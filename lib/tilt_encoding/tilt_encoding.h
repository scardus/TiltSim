#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

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

// Readings refresh every 5 s, matching the manufacturer's stated interval: the
// values are drawn once per cycle and every colour keeps them for the whole
// cycle, however many times it airs.
constexpr unsigned long kCyclePeriodMs = 5000;

// How long one colour holds the radio before the rotation moves on.
//
// Each colour is aired repeatedly within a cycle rather than once, because a
// receiver that builds a discovery list per scan can only list a colour it
// actually heard inside that window. The HM-10 used downstream defaults to a
// 3 s scan, so one 5 s-spaced burst per colour is missed more often than not --
// which is what made colours appear to alternate exclusively. At 200 ms a full
// eight-colour rotation is 1.6 s, so every colour lands in a 3 s window at
// least once, and the usual four-colour setup rotates in 800 ms.
//
// This is a deliberate departure from real Tilt behaviour, which bursts once per
// 5 s from its own radio. One radio cannot advertise eight addresses at once
// (concurrent advertising sets need BLE 5.0 extended advertising, which the
// classic ESP32's Bluedroid stack does not provide), so the duty cycle is traded
// for discoverability. The reading cadence stays honest at one value per 5 s.
constexpr unsigned long kSliceMs = 200;

// BLE Core Spec advDelay: 0-10 ms of pseudo-random jitter per advertising event.
constexpr unsigned long kAdvDelayMaxMs = 10;

// The manufacturer-specific payload, byte for byte as it goes on air:
//
//   4C 00        Apple company ID, little-endian per the AD-data convention
//   02 15        iBeacon subtype and length, both constant
//   16x UUID     canonical order, so byte 3 is the Tilt colour
//   MM MM        major, BIG endian
//   mm mm        minor, BIG endian
//   PP           measured (TX) power
//
// This is built explicitly rather than via BLEBeacon because that class stores
// the fields in a packed struct and byte-swaps every 16-bit setter, so the value
// you pass is not the value that airs: it wants 0x4C00 to emit Apple's 0x004C.
// Getting that backwards emitted company ID 0x4C00 for real, which is not Apple
// and is not what a Tilt sends. Assembling the bytes here keeps the on-air
// order under test instead of buried in an Arduino-only translation unit.
constexpr size_t kIBeaconPayloadLen = 25;
using IBeaconPayload = std::array<uint8_t, kIBeaconPayloadLen>;

// Returns false, leaving out untouched, if canonicalUuid is not a valid 128-bit
// UUID. Major and minor are the already-encoded raw field values.
bool buildIBeaconPayload(const char* canonicalUuid, uint16_t major,
                         uint16_t minor, int8_t txPower, IBeaconPayload& out);

// Pro Tilts report one more decimal place by advertising 10x the standard
// value: 68.5 degF -> major 685, 1.0530 SG -> minor 10530.
//
// The offset is the already-drawn variance in real units. Keeping the draw in
// the caller leaves these functions pure and deterministic to test. Applying it
// before scaling is what makes a +/-2 degF variance move a Pro's major by +/-20
// rather than +/-2.
uint16_t encodeTemperature(float tempF, float offsetF, bool pro);
uint16_t encodeGravity(float gravity, float offset, bool pro);

// How long one full pass through the enabled colours takes. A receiver's scan
// window must exceed this to be certain of seeing every colour.
unsigned long rotationDurationMs(size_t activeCount);

// Each simulated Tilt advertises from its own address, because a scanner that
// keys its discovery list on the address collapses every colour into one entry
// otherwise -- the receiver then sees a single Tilt whose UUID keeps changing,
// and reports whichever colour happened to land last in the scan window.
//
// These are BLE *static random* addresses, which the Core Spec requires to have
// the two most significant bits set. Derived from the chip's own MAC so they are
// stable across restarts (readings stay traceable between runs) and unique to
// the physical device, with the colour index in the low bits so all eight are
// guaranteed distinct.
constexpr size_t kBleAddressLen = 6;
using BleAddress = std::array<uint8_t, kBleAddressLen>;

// baseMac must point to kBleAddressLen bytes. Returns false, leaving out
// untouched, for a null pointer or a colourIndex past the last colour.
bool tiltBleAddress(const uint8_t* baseMac, size_t colourIndex, BleAddress& out);
