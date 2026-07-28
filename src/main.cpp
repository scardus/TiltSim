#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <esp_gap_ble_api.h>
#include <esp_random.h>
#include <esp_system.h>

#include <string>

#include "heap.h"
#include "net.h"
#include "tilt_config.h"
#include "tilt_encoding.h"
#include "version.h"
#include "web_server.h"

namespace {
constexpr int8_t kMeasuredPower = -10;

BLEAdvertising* gAdvertising = nullptr;
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
// slice is a single esp_ble_gap_config_adv_data_raw() call with nothing to
// assemble and nothing to allocate.
AdvData gAdvData[kTiltCount];
BleAddress gAddresses[kTiltCount];

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

float randomOffset(const float range) {
  if (range <= 0.0f) {
    return 0.0f;
  }
  // random() is integer-only, so work in thousandths of the range.
  const long steps = random(-1000, 1001);
  return range * (static_cast<float>(steps) / 1000.0f);
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
        encodeTemperature(tilt.tempF, randomOffset(tilt.tempVarianceF), tilt.pro);
    const uint16_t minor =
        encodeGravity(tilt.gravity, randomOffset(tilt.gravityVariance), tilt.pro);

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

  // Must happen while stopped: the controller will not change the random
  // address out from under an active advertisement.
  gAdvertising->setDeviceAddress(gAddresses[colourIndex].data(),
                                 BLE_ADDR_TYPE_RANDOM);

  // Straight to the controller. The bytes were assembled at the cycle boundary,
  // so this whole slice allocates nothing -- where going through
  // BLEAdvertisementData took seven heap allocations every 200 ms. See
  // buildAdvData() for the detail, and setup() for why start() below does not
  // overwrite what this just configured.
  const esp_err_t rc = esp_ble_gap_config_adv_data_raw(
      gAdvData[colourIndex].data(), gAdvData[colourIndex].size());
  if (rc != ESP_OK) {
    Serial.printf("BLE: could not set advertising data for %s (rc=%d)\n",
                  kTiltColours[colourIndex].name, rc);
    return false;
  }

  gAdvertising->start();
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
  gAdvertising->stop();
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

  BleAddress baseMac;
  efuseMacBytes(ESP.getEfuseMac(), baseMac);
  for (size_t i = 0; i < kTiltCount; ++i) {
    if (!tiltBleAddress(baseMac.data(), i, gAddresses[i])) {
      Serial.printf("Could not derive an address for %s\n", kTiltColours[i].name);
    }
  }

  BLEDevice::init("ESP32-iBeacon");
  gAdvertising = BLEDevice::getAdvertising();
  gAdvertising->setScanResponse(false);

  // A Tilt is a beacon, not something you connect to. The library default is
  // ADV_TYPE_IND, which is connectable, so without this a receiver could open a
  // connection and take the radio away mid-rotation. SCAN_IND is
  // non-connectable but still scannable, so an active scanner's SCAN_REQ is
  // still answered -- with an empty response, since scan data is off above.
  //
  // Set once: start() passes m_advParams every slice, and neither
  // setDeviceAddress() nor setAdvertisementData() disturbs the type.
  gAdvertising->setAdvertisementType(ADV_TYPE_SCAN_IND);

  // Claim ownership of the advertising data, once, so the slices below can talk
  // to the controller directly.
  //
  // BLEAdvertising::start() reconfigures the data itself unless m_customAdvData
  // is set (BLEAdvertising.cpp:211) -- it would push its own struct, complete
  // with the device name, straight over whatever
  // esp_ble_gap_config_adv_data_raw() had just written, every single slice. That
  // flag is private with no setter, and calling setAdvertisementData() is the
  // only thing that turns it on. So it is called exactly once here, with the
  // real first-colour bytes, purely for its side effect; from then on every
  // slice writes raw and start() leaves the data alone.
  BLEAdvertisementData claimCustomData;
  claimCustomData.setFlags(kAdvFlags);
  claimCustomData.setManufacturerData(std::string());
  gAdvertising->setAdvertisementData(claimCustomData);

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
