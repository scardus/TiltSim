#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLEBeacon.h>
#include <BLEDevice.h>

#include "tilt_encoding.h"

namespace {
struct BeaconConfig {
  const char* uuid;
  int baseMajorDegF;
  uint16_t minor;
  bool active;
};

constexpr BeaconConfig kBeacons[] = {
  {"a495bb10-c5b1-4b44-b512-1370f02d74de", 64, 1051, true},   // Red
  {"a495bb20-c5b1-4b44-b512-1370f02d74de", 66, 1052, true},   // Green
  {"a495bb30-c5b1-4b44-b512-1370f02d74de", 680, 10530, true},   // Black (pro)
  {"a495bb40-c5b1-4b44-b512-1370f02d74de", 69, 1054, false},   // Purple
  {"a495bb50-c5b1-4b44-b512-1370f02d74de", 71, 1055, false},   // Orange
  {"a495bb60-c5b1-4b44-b512-1370f02d74de", 73, 1056, false},   // Blue
  {"a495bb70-c5b1-4b44-b512-1370f02d74de", 75, 1057, false},   // Yellow
  {"a495bb80-c5b1-4b44-b512-1370f02d74de", 77, 1058, false},   // Pink
};

constexpr int8_t kMeasuredPower = -10;
constexpr unsigned long kRunPeriodMs = 5000;
constexpr unsigned long kAdvertiseWindowMs = 1000;

BLEAdvertising* gAdvertising = nullptr;
unsigned long gLastRunMs = 0;
unsigned long gAdvertiseStartMs = 0;
bool gIsAdvertising = false;
bool gRunActive = false;
size_t gCurrentBeaconIndex = 0;
int gRunVarianceDegF = 0;

uint16_t encodeMajorDegFWithVariance(const int baseMajorDegF, const int varianceDegF) {
  const int adjusted = baseMajorDegF + varianceDegF;
  if (adjusted < 0) {
    return 0;
  }
  return static_cast<uint16_t>(adjusted);
}

size_t findNextActiveBeaconIndex(const size_t startIndex) {
  const size_t beaconCount = sizeof(kBeacons) / sizeof(kBeacons[0]);
  for (size_t i = startIndex; i < beaconCount; ++i) {
    if (kBeacons[i].active) {
      return i;
    }
  }
  return beaconCount;
}
}

void startBeaconAdvertisement(const size_t beaconIndex) {
  const size_t beaconCount = sizeof(kBeacons) / sizeof(kBeacons[0]);
  if (beaconCount == 0 || beaconIndex >= beaconCount) {
    return;
  }

  const BeaconConfig& cfg = kBeacons[beaconIndex];
  const char* canonicalUuid = cfg.uuid;
  const std::string beaconUuid = canonicalToBleBeaconUuidInput(canonicalUuid);
  if (beaconUuid.empty()) {
    Serial.print("Invalid UUID: ");
    Serial.println(canonicalUuid);
    return;
  }

  const uint16_t major = encodeMajorDegFWithVariance(cfg.baseMajorDegF, gRunVarianceDegF);

  BLEBeacon beacon;
  beacon.setManufacturerId(0x004C); // Apple company identifier for iBeacon format
  beacon.setProximityUUID(BLEUUID(beaconUuid));
  beacon.setMajor(major);
  beacon.setMinor(cfg.minor);
  beacon.setSignalPower(kMeasuredPower);

  BLEAdvertisementData advData;
  advData.setFlags(0x04);
  advData.setManufacturerData(beacon.getData());

  gAdvertising->setAdvertisementData(advData);
  gAdvertising->start();
  gAdvertiseStartMs = millis();
  gIsAdvertising = true;

  Serial.print("iBeacon advertising started for UUID: ");
  Serial.println(canonicalUuid);
  Serial.print("  major(degF): ");
  Serial.println(major);
  Serial.print("  minor(gravity): ");
  Serial.println(cfg.minor);
}

void stopIBeaconAdvertisement() {
  gAdvertising->stop();
  gIsAdvertising = false;
  Serial.println("iBeacon advertising stopped");
}

void setup() {
  Serial.begin(115200);
  randomSeed(static_cast<unsigned long>(micros()));
  BLEDevice::init("ESP32-iBeacon");
  gAdvertising = BLEDevice::getAdvertising();
  gAdvertising->setScanResponse(false);

  // Trigger the first run immediately after boot.
  gLastRunMs = millis() - kRunPeriodMs;
}

void loop() {
  const unsigned long now = millis();
  const size_t beaconCount = sizeof(kBeacons) / sizeof(kBeacons[0]);

  if (!gRunActive && !gIsAdvertising && (now - gLastRunMs >= kRunPeriodMs)) {
    gRunVarianceDegF = random(-2, 3);
    gRunActive = true;
    gCurrentBeaconIndex = findNextActiveBeaconIndex(0);
    Serial.print("Starting run with variance (degF): ");
    Serial.println(gRunVarianceDegF);
    if (gCurrentBeaconIndex < beaconCount) {
      startBeaconAdvertisement(gCurrentBeaconIndex);
    } else {
      gRunActive = false;
      gLastRunMs = millis();
      Serial.println("No active beacons; run skipped");
    }
  }

  if (gIsAdvertising && (now - gAdvertiseStartMs >= kAdvertiseWindowMs)) {
    stopIBeaconAdvertisement();

    if (gRunActive) {
      gCurrentBeaconIndex = findNextActiveBeaconIndex(gCurrentBeaconIndex + 1);
      if (gCurrentBeaconIndex < beaconCount) {
        startBeaconAdvertisement(gCurrentBeaconIndex);
      } else {
        gRunActive = false;
        gLastRunMs = millis();
        Serial.println("Run complete");
      }
    }
  }
}