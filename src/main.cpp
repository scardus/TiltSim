#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>

#include <string>

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

// Both indexed by colour, not by slot. Payloads are built once per cycle so a
// colour advertises identical readings every time it airs within that cycle,
// keeping the value cadence at one reading per 5 s even though the rotation
// visits each colour several times.
IBeaconPayload gPayloads[kTiltCount];
BleAddress gAddresses[kTiltCount];

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
  gScheduleCount = 0;
  if (!configLock()) {
    return;
  }
  if (gConfig.masterEnabled) {
    for (size_t i = 0; i < kTiltCount; ++i) {
      if (gConfig.tilts[i].enabled) {
        gSchedule[gScheduleCount++] = i;
      }
    }
  }
  const AppConfig config = gConfig;
  configUnlock();

  for (size_t slot = 0; slot < gScheduleCount; ++slot) {
    const size_t colourIndex = gSchedule[slot];
    const TiltSettings& tilt = config.tilts[colourIndex];

    const uint16_t major =
        encodeTemperature(tilt.tempF, randomOffset(tilt.tempVarianceF), tilt.pro);
    const uint16_t minor =
        encodeGravity(tilt.gravity, randomOffset(tilt.gravityVariance), tilt.pro);

    if (!buildIBeaconPayload(kTiltColours[colourIndex].uuid, major, minor,
                             kMeasuredPower, gPayloads[colourIndex])) {
      Serial.print("Invalid UUID for ");
      Serial.println(kTiltColours[colourIndex].name);
      continue;
    }

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

  BLEAdvertisementData advData;
  advData.setFlags(0x04);
  // The length-taking constructor is required: the payload contains 0x00 bytes.
  advData.setManufacturerData(
      std::string(reinterpret_cast<const char*>(gPayloads[colourIndex].data()),
                  gPayloads[colourIndex].size()));

  gAdvertising->setAdvertisementData(advData);
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
}  // namespace

void setup() {
  Serial.begin(115200);
  randomSeed(static_cast<unsigned long>(micros()));

  Serial.printf("\n%s %s (%s)\n", FW_NAME, FW_VERSION, FW_BUILD_DATE);

  configBegin();
  configPrint();

  // Before BLE: the captive portal blocks, and there is no point holding the
  // BLE stack's memory while it does.
  if (netBegin()) {
    webServerBegin();
  }

  // ESP.getEfuseMac() packs the factory MAC with byte 0 in the low bits, so this
  // reads out in the same order the board prints on its label.
  const uint64_t efuseMac = ESP.getEfuseMac();
  uint8_t baseMac[kBleAddressLen];
  for (size_t i = 0; i < kBleAddressLen; ++i) {
    baseMac[i] = static_cast<uint8_t>((efuseMac >> (8 * i)) & 0xFF);
  }
  for (size_t i = 0; i < kTiltCount; ++i) {
    if (!tiltBleAddress(baseMac, i, gAddresses[i])) {
      Serial.printf("Could not derive an address for %s\n", kTiltColours[i].name);
    }
  }

  BLEDevice::init("ESP32-iBeacon");
  gAdvertising = BLEDevice::getAdvertising();
  gAdvertising->setScanResponse(false);

  Serial.printf("BLE: %u colours rotate every %lu ms, readings refresh every %lu ms\n",
                static_cast<unsigned>(kTiltCount), kSliceMs, kCyclePeriodMs);

  // Trigger the first cycle immediately after boot.
  gCycleStartMs = millis() - kCyclePeriodMs;
}

void loop() {
  const unsigned long now = millis();

  configFlushIfDue();
  netLoop();
  webServerLoop();

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
