#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <esp_system.h>

#include "heap.h"
#include "ispindel.h"
#include "net.h"
#include "ota_rollback.h"
#include "tilt_config.h"
#include "tilt_encoding.h"
#include "version.h"
#include "web_server.h"

namespace {
constexpr int8_t kMeasuredPower = -10;

// Filled in once by setup() and handed to every ble_gap_adv_start() unchanged.
ble_gap_adv_params gAdvParams;

unsigned long gCycleStartMs = 0;
unsigned long gSliceStartMs = 0;
unsigned long gCycleLengthMs = kCyclePeriodMs;
size_t gSlotIndex = 0;
bool gIsAdvertising = false;

// The colours enabled for the current cycle, snapshotted at cycle start so a
// web edit mid-cycle cannot reshuffle the slots underneath us.
size_t gSchedule[kTiltCount];
size_t gScheduleCount = 0;

// Both indexed by colour, not by slot. The advertisement is built once per cycle
// so a colour advertises identical readings every time it airs within that
// cycle, keeping the value cadence at one reading per 5 s even though the
// rotation visits each colour several times.
//
// Stored as the finished on-air bytes rather than the iBeacon payload, so a
// slice is a single ble_gap_adv_set_data() call with nothing to assemble and
// nothing to allocate.
AdvData gAdvData[kTiltCount];

// gAddresses is the printed order, which is what the per-cycle log shows;
// gAddressesLe is the same addresses reversed for the stack. Both are derived
// once at boot -- see bleAddressLittleEndian() for why the two exist.
BleAddress gAddresses[kTiltCount];
BleAddress gAddressesLe[kTiltCount];

// Often enough to watch a leak develop, rare enough not to bury the per-cycle
// colour lines.
constexpr unsigned long kHealthIntervalMs = 30000;
unsigned long gLastHealthMs = 0;

// Periodic health line. Heap is the tighter constraint on this device than
// flash: serving the web UI needs contiguous kilobytes, and running out of them
// presents as the server hanging rather than as an error, so it is worth being
// able to watch the numbers move.
void logHealth(const unsigned long now) {
  if (now - gLastHealthMs < kHealthIntervalMs) {
    return;
  }
  gLastHealthMs = now;

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largest = largestUsableBlock();
  // ESP32's core has no getHeapFragmentation(), so derive it: the share of free
  // heap that is not in one piece. A high number means an allocation well under
  // the free total can still fail, which is exactly the failure that is hard to
  // diagnose from the outside.
  const uint32_t fragmentation =
      freeHeap > 0 ? 100 - (largest * 100 / freeHeap) : 0;
  // The low-water mark, because a reading taken after the fact misses a
  // transient dip -- and a transient dip is what a burst of requests causes.
  const uint32_t minEver = ESP.getMinFreeHeap();

  if (netIsConnected()) {
    Serial.printf("[HEALTH] Free heap: %u bytes, Largest contiguous: %u bytes, "
                  "Fragmentation: %u%%, Min ever: %u bytes | WiFi RSSI: %d dBm\n",
                  freeHeap, largest, fragmentation, minEver, netRssi());
  } else {
    Serial.printf("[HEALTH] Free heap: %u bytes, Largest contiguous: %u bytes, "
                  "Fragmentation: %u%%, Min ever: %u bytes | WiFi: offline\n",
                  freeHeap, largest, fragmentation, minEver);
  }
}

// Snapshots the enabled colours and draws one reading each for the cycle ahead.
// Logged here rather than per slice: the rotation re-airs each colour every few
// hundred ms, and logging that would bury everything else.
void buildSchedule() {
  // The lock is only contended when a request is being served, and going silent
  // because someone opened the web UI is a worse answer than airing the last
  // cycle's readings again. Leave gSchedule and gScheduleCount alone and retry
  // at the next boundary.
  if (!configLock()) {
    Serial.println("BLE: config busy, reusing the previous cycle");
    return;
  }
  const AppConfig config = gConfig;
  configUnlock();

  gScheduleCount = 0;
  if (config.masterEnabled) {
    for (size_t i = 0; i < kTiltCount; ++i) {
      if (config.tilts[i].enabled) {
        gSchedule[gScheduleCount++] = i;
      }
    }
  }

  for (size_t slot = 0; slot < gScheduleCount; ++slot) {
    const size_t colourIndex = gSchedule[slot];
    const TiltSettings& tilt = config.tilts[colourIndex];

    const uint16_t major =
        encodeTemperature(tilt.tempF, randomVariance(tilt.tempVarianceF), tilt.pro);
    const uint16_t minor =
        encodeGravity(tilt.gravity, randomVariance(tilt.gravityVariance), tilt.pro);

    IBeaconPayload payload;
    if (!buildIBeaconPayload(kTiltColours[colourIndex].uuid, major, minor,
                             kMeasuredPower, payload)) {
      Serial.print("Invalid UUID for ");
      Serial.println(kTiltColours[colourIndex].name);
      continue;
    }
    buildAdvData(payload, gAdvData[colourIndex]);

    const BleAddress& mac = gAddresses[colourIndex];
    Serial.printf("%-6s %s major=%-5u minor=%-6u mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  kTiltColours[colourIndex].name, tilt.pro ? "pro" : "std", major,
                  minor, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  // BLE Core Spec advDelay: a pseudo-random 0-10 ms is added to each
  // advertising event. The controller already does this between packets within
  // a slice; this covers the cycle boundary so the simulator does not sit on a
  // suspiciously exact 5000 ms period.
  gCycleLengthMs = kCyclePeriodMs + random(0, kAdvDelayMaxMs + 1);
}

bool startSlot(const size_t slotIndex) {
  if (slotIndex >= gScheduleCount) {
    return false;
  }
  const size_t colourIndex = gSchedule[slotIndex];
  const char* const name = kTiltColours[colourIndex].name;

  // Must happen while stopped: the controller will not change the random
  // address out from under an active advertisement. Checked rather than fired
  // and forgotten -- a rejected address leaves the previous colour's one in
  // place, so every colour would air from one address and the receiver would
  // collapse them all into a single Tilt. That is the exact fault the
  // per-colour address exists to prevent, and it looks like healthy firmware.
  if (!NimBLEDevice::setOwnAddr(gAddressesLe[colourIndex].data())) {
    Serial.printf("BLE: could not set the address for %s\n", name);
    return false;
  }

  // Straight to the controller: ble_gap_adv_set_data() is a thin wrapper on the
  // HCI Set Advertising Data command and sends exactly these bytes, flags
  // structure included. The bytes were assembled at the cycle boundary, so this
  // whole slice allocates nothing -- where the advertisement-builder class this
  // replaced took seven heap allocations every 200 ms. See buildAdvData().
  int rc = ble_gap_adv_set_data(gAdvData[colourIndex].data(),
                                static_cast<int>(gAdvData[colourIndex].size()));
  if (rc != 0) {
    Serial.printf("BLE: could not set advertising data for %s (rc=%d)\n", name,
                  rc);
    return false;
  }

  // No GAP event callback: a non-connectable advertisement running until it is
  // stopped by hand produces no events to receive.
  rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, nullptr, BLE_HS_FOREVER,
                         &gAdvParams, nullptr, nullptr);
  if (rc != 0) {
    Serial.printf("BLE: could not start advertising %s (rc=%d)\n", name, rc);
    return false;
  }

  gSliceStartMs = millis();
  gIsAdvertising = true;
  return true;
}

// Advances to the next slot that actually airs, wrapping within the cycle.
// Bounded by the schedule length so a schedule of entirely unusable slots
// cannot spin here.
void startNextUsableSlot() {
  for (size_t attempts = 0; attempts < gScheduleCount; ++attempts) {
    if (startSlot(gSlotIndex)) {
      return;
    }
    gSlotIndex = (gSlotIndex + 1) % gScheduleCount;
  }
}

void stopSlot() {
  if (!gIsAdvertising) {
    return;
  }
  const int rc = ble_gap_adv_stop();
  if (rc != 0) {
    Serial.printf("BLE: advertising would not stop (rc=%d)\n", rc);
  }
  // Cleared either way. If the controller is already stopped the flag was the
  // thing that was wrong, and holding it true would wedge the rotation.
  gIsAdvertising = false;
}
// Why the device last restarted. This reboots for at least five different
// reasons -- a portal save, /api/reboot, /api/reset-wifi, an OTA install, or a
// panic -- and until now the log looked identical for all of them, which made
// "did it crash or did someone press the button?" unanswerable after the fact.
const char* resetReasonName(const esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_EXT:      return "external reset";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "unknown";
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  // Hardware-backed, unlike micros(): the boot path to this line is near enough
  // constant that every board drew almost the same variance sequence.
  randomSeed(esp_random());

  Serial.printf("\n%s %s (%s)\n", FW_NAME, FW_VERSION, FW_BUILD_DATE);

  const esp_reset_reason_t resetReason = esp_reset_reason();
  Serial.printf("Boot: reset reason %s\n", resetReasonName(resetReason));

  // Early, and before anything that can fail: this is the half of "why did it
  // boot like that" the reset reason cannot answer, and a panic further down
  // setup() is precisely the case it exists to recover from.
  otaRollbackBegin();

  configBegin();
  configPrint();

  // Warm the sketch MD5 while the heap is quiet and nothing is being served.
  //
  // /api/state reports it, and the first call is the single largest allocation
  // anywhere in the web path: getSketchMD5() takes a 4 KB contiguous buffer to
  // read the whole image through (Esp.cpp:222). Asking for that on a heap
  // squeezed by several concurrent connections is a poor bet -- operator new
  // throws, nothing in the handler chain catches it, and std::terminate calls
  // abort(). The result is cached in a function-static String, so paying for it
  // here means every later call is free.
  const unsigned long md5Start = millis();
  const String sketchMd5 = ESP.getSketchMD5();
  Serial.printf("Boot: sketch md5 %s (%lu ms)\n", sketchMd5.c_str(),
                millis() - md5Start);

  // Before BLE: the captive portal blocks, and there is no point holding the
  // BLE stack's memory while it does.
  if (netBegin()) {
    webServerBegin();
  }

  // After the network, because posting without one is pointless, and before
  // NimBLEDevice::init() below, because that is the roomiest the heap ever is
  // and this is where the posting task's stack has to come from.
  ispindelBegin();

  BleAddress baseMac;
  efuseMacBytes(ESP.getEfuseMac(), baseMac);
  for (size_t i = 0; i < kTiltCount; ++i) {
    if (!tiltBleAddress(baseMac.data(), i, gAddresses[i])) {
      Serial.printf("Could not derive an address for %s\n", kTiltColours[i].name);
    }
    bleAddressLittleEndian(gAddresses[i], gAddressesLe[i]);
  }

  // An empty name on purpose: it never airs. Scan response data is never set,
  // and the advertisement is 30 of the 31 available bytes of iBeacon payload,
  // so there is nowhere for a name to go even if one were wanted.
  //
  // init() does not return until the host and controller have synced, so the
  // ble_gap_* calls below are safe from the next line onwards.
  if (!NimBLEDevice::init("")) {
    Serial.println("BLE: the stack would not start; no advertising this boot");
  }

  // A Tilt is a beacon, not something you connect to. NimBLE derives the
  // advertising PDU type from these two rather than taking it by name:
  // ble_gap_adv_type() in ble_gap.c maps CONN_MODE_NON with a discoverable
  // disc_mode to ADV_SCAN_IND, and only pairs it with DISC_MODE_NON to get
  // ADV_NONCONN_IND. So this is the same SCAN_IND the Bluedroid build aired --
  // non-connectable, so nothing can open a connection and take the radio away
  // mid-rotation, but still scannable, so an active scanner's SCAN_REQ is
  // answered with an empty response.
  gAdvParams = {};
  gAdvParams.conn_mode = BLE_GAP_CONN_MODE_NON;
  gAdvParams.disc_mode = BLE_GAP_DISC_MODE_GEN;

  // Set explicitly because NimBLE's defaults are not Bluedroid's. Left at zero
  // the host substitutes its own slower pair, which would thin out the packets
  // sent inside each 200 ms slice -- and the number of packets a colour gets
  // into a receiver's scan window is the whole point of the rotation. 0x20 and
  // 0x40 are 20 ms and 40 ms, matching what BLEAdvertising used to default to.
  gAdvParams.itvl_min = 0x20;
  gAdvParams.itvl_max = 0x40;

  Serial.printf("BLE: %u colours rotate every %lu ms, readings refresh every %lu ms\n",
                static_cast<unsigned>(kTiltCount), kSliceMs, kCyclePeriodMs);

  // Trigger the first cycle immediately after boot.
  gCycleStartMs = millis() - kCyclePeriodMs;
}

// Deliberately never calls delay() or yield(), which looks like an oversight and
// is not. Two reasons it is safe: the task watchdog does not watch the core 1
// idle task (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 is unset in the Arduino
// sdkconfig, precisely because loop() is expected to spin), and the AsyncTCP
// task runs at priority 10 against this task's 1, so it preempts regardless.
// Adding a delay(1) here would only add jitter to the slice timing.
void loop() {
  const unsigned long now = millis();

  configFlushIfDue();
  netLoop();
  webServerLoop();
  otaRollbackLoop();
  logHealth(now);

  // Leave the radio and CPU to the upload; it reboots when it finishes.
  if (webOtaInProgress()) {
    stopSlot();
    return;
  }

  // Cycle boundary: re-read the config and draw fresh readings. Checked before
  // the slice advance so a colour cannot be started only to be stopped again in
  // the same pass.
  if (now - gCycleStartMs >= gCycleLengthMs) {
    stopSlot();
    gCycleStartMs = now;
    buildSchedule();
    gSlotIndex = 0;
    startNextUsableSlot();
    return;
  }

  // Rotate to the next colour. Unlike the old one-burst-per-cycle schedule this
  // wraps and keeps going for the whole cycle, so every colour reappears often
  // enough to land in any receiver scan window.
  if (gIsAdvertising && (now - gSliceStartMs >= kSliceMs)) {
    stopSlot();
    if (gScheduleCount > 0) {
      gSlotIndex = (gSlotIndex + 1) % gScheduleCount;
    }
    startNextUsableSlot();
  }
}
