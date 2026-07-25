#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLEBeacon.h>
#include <BLEDevice.h>

#include "net.h"
#include "tilt_config.h"
#include "tilt_encoding.h"
#include "version.h"

namespace {
constexpr int8_t kMeasuredPower = -10;

BLEAdvertising* gAdvertising = nullptr;
unsigned long gCycleStartMs = 0;
unsigned long gSliceStartMs = 0;
unsigned long gSliceMs = 0;
unsigned long gCycleLengthMs = kCyclePeriodMs;
size_t gSlotIndex = 0;
bool gIsAdvertising = false;

// The colours enabled for the current cycle, snapshotted at cycle start so a
// web edit mid-cycle cannot reshuffle the slots underneath us.
size_t gSchedule[kTiltCount];
size_t gScheduleCount = 0;

float randomOffset(const float range) {
  if (range <= 0.0f) {
    return 0.0f;
  }
  // random() is integer-only, so work in thousandths of the range.
  const long steps = random(-1000, 1001);
  return range * (static_cast<float>(steps) / 1000.0f);
}

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
  configUnlock();

  gSliceMs = sliceDurationMs(gScheduleCount);
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

  if (!configLock()) {
    return false;
  }
  const TiltSettings tilt = gConfig.tilts[colourIndex];
  configUnlock();

  const std::string beaconUuid =
      canonicalToBleBeaconUuidInput(kTiltColours[colourIndex].uuid);
  if (beaconUuid.empty()) {
    Serial.print("Invalid UUID for ");
    Serial.println(kTiltColours[colourIndex].name);
    return false;
  }

  const uint16_t major =
      encodeTemperature(tilt.tempF, randomOffset(tilt.tempVarianceF), tilt.pro);
  const uint16_t minor =
      encodeGravity(tilt.gravity, randomOffset(tilt.gravityVariance), tilt.pro);

  BLEBeacon beacon;
  beacon.setManufacturerId(0x004C); // Apple company identifier for iBeacon format
  beacon.setProximityUUID(BLEUUID(beaconUuid));
  beacon.setMajor(major);
  beacon.setMinor(minor);
  beacon.setSignalPower(kMeasuredPower);

  BLEAdvertisementData advData;
  advData.setFlags(0x04);
  advData.setManufacturerData(beacon.getData());

  gAdvertising->setAdvertisementData(advData);
  gAdvertising->start();
  gSliceStartMs = millis();
  gIsAdvertising = true;

  Serial.printf("%-6s %s major=%u minor=%u\n", kTiltColours[colourIndex].name,
                tilt.pro ? "pro" : "std", major, minor);
  return true;
}

// Advances to the next slot that actually airs, so one bad slot does not stall
// the remainder of the cycle.
void startNextUsableSlot() {
  while (gSlotIndex < gScheduleCount && !startSlot(gSlotIndex)) {
    ++gSlotIndex;
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
  netBegin();

  BLEDevice::init("ESP32-iBeacon");
  gAdvertising = BLEDevice::getAdvertising();
  gAdvertising->setScanResponse(false);

  // Trigger the first cycle immediately after boot.
  gCycleStartMs = millis() - kCyclePeriodMs;
}

void loop() {
  const unsigned long now = millis();

  configFlushIfDue();
  netLoop();

  // End the current slot, then either start the next one or fall quiet for the
  // rest of the cycle.
  if (gIsAdvertising && (now - gSliceStartMs >= gSliceMs)) {
    stopSlot();
    ++gSlotIndex;
    startNextUsableSlot();
  }

  if (!gIsAdvertising && (now - gCycleStartMs >= gCycleLengthMs)) {
    gCycleStartMs = now;
    buildSchedule();
    gSlotIndex = 0;
    startNextUsableSlot();
  }
}
