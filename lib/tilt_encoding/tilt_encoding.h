#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Arduino-free Tilt payload logic. Everything here is pure so it can be
// exercised by the host-side unit tests in test/test_tilt_encoding.

struct TiltColour {
  const char* name;
  const char* uuid;
  const char* swatch;  // CSS hex used by the web UI
};

constexpr TiltColour kTiltColours[] = {
  {"Red",    "a495bb10-c5b1-4b44-b512-1370f02d74de", "#e8342e"},
  {"Green",  "a495bb20-c5b1-4b44-b512-1370f02d74de", "#3aa655"},
  {"Black",  "a495bb30-c5b1-4b44-b512-1370f02d74de", "#2b2b2b"},
  {"Purple", "a495bb40-c5b1-4b44-b512-1370f02d74de", "#8b3fa8"},
  {"Orange", "a495bb50-c5b1-4b44-b512-1370f02d74de", "#f07c1e"},
  {"Blue",   "a495bb60-c5b1-4b44-b512-1370f02d74de", "#2b7cd3"},
  {"Yellow", "a495bb70-c5b1-4b44-b512-1370f02d74de", "#f2c400"},
  {"Pink",   "a495bb80-c5b1-4b44-b512-1370f02d74de", "#e86aa6"},
};

constexpr size_t kTiltCount = sizeof(kTiltColours) / sizeof(kTiltColours[0]);

// Advertising cadence. Real Tilts wake roughly every 5 s, emit a burst and
// sleep, so every enabled colour must air exactly once per cycle regardless of
// how many are enabled.
constexpr unsigned long kCyclePeriodMs = 5000;
constexpr unsigned long kMaxSliceMs = 1000;
// BLE Core Spec advDelay: 0-10 ms of pseudo-random jitter per advertising event.
constexpr unsigned long kAdvDelayMaxMs = 10;

// BLEBeacon::setProximityUUID memcpy's the little-endian uuid128 straight into
// the payload, so the string handed to BLEUUID must be byte-reversed to put the
// canonical UUID on air. Returns "" if the input is not a valid 128-bit UUID.
std::string canonicalToBleBeaconUuidInput(const char* canonicalUuid);

// Pro Tilts report one more decimal place by advertising 10x the standard
// value: 68.5 degF -> major 685, 1.0530 SG -> minor 10530.
//
// The offset is the already-drawn variance in real units. Keeping the draw in
// the caller leaves these functions pure and deterministic to test. Applying it
// before scaling is what makes a +/-2 degF variance move a Pro's major by +/-20
// rather than +/-2.
uint16_t encodeTemperature(float tempF, float offsetF, bool pro);
uint16_t encodeGravity(float gravity, float offset, bool pro);

// Length of one colour's advertising slot, given how many are enabled.
unsigned long sliceDurationMs(size_t activeCount);
