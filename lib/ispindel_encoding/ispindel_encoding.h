#pragma once

#include <cstddef>
#include <cstdint>

// iSpindel / Gravitymon payload logic, kept apart from the Arduino layer so the
// unit tests in test/test_ispindel_encoding can exercise it directly.
//
// An iSpindel is the mirror image of a Tilt: rather than advertising passively
// over BLE it wakes on a timer, HTTP POSTs a JSON body to an endpoint the user
// configures, and sleeps again. Only temperature and gravity are simulated;
// every other field is a fixed placeholder, defined below so the one file that
// describes the wire format is the file that holds the values.

constexpr size_t kIspindelCount = 4;

// Six hex characters plus the terminator, matching the "2E6753" form real
// devices derive from their chip ID.
constexpr size_t kIspindelIdLen = 7;

// Reported every time, and matching the actual posting cadence in
// src/ispindel.cpp. A receiver that spaces its own expectations off this value
// would otherwise mark the device late.
constexpr unsigned long kIspindelIntervalSec = 900;

// Placeholders. These are plausible rather than meaningful: nothing in the
// simulator models a battery, a tilt angle or a real link budget.
constexpr const char* kPlaceholderToken = "gravmon";
constexpr float kPlaceholderAngle = 45.34f;
constexpr float kPlaceholderBattery = 3.67f;
constexpr int kPlaceholderRssi = -12;

// Temperatures always go out in Fahrenheit, so temp_units is always "F".
//
// A real iSpindel can be configured either way, but the simulator stores degF
// throughout -- that is what a Tilt advertises, and keeping one internal unit
// avoids a second conversion path that could disagree with the first. The web
// UI converts for display only, exactly as it already does for the Tilts.
constexpr const char* kIspindelTempUnits = "F";

// Derives the six-character device ID for a slot from the board's own MAC.
//
// Same trick as tiltBleAddress(): the chip's address with the slot index in the
// low bits. Stable across restarts so a receiver keeps recognising the device,
// unique to the physical board so two simulators do not collide, and distinct
// per slot so all four look like separate devices. baseMac must point to at
// least six bytes.
//
// Returns false, leaving out untouched, for a null pointer, a slot past the
// last one, or a buffer shorter than kIspindelIdLen.
bool ispindelId(const uint8_t* baseMac, size_t index, char* out, size_t outLen);

// The values that vary per post. Strings are borrowed, not copied, so they must
// outlive the call.
struct IspindelReading {
  const char* name;
  const char* id;
  float tempF;
  float gravity;
};

// Serialises one reading into the JSON body a real device would POST. Returns
// the number of characters written, or 0 if the buffer was too small -- in
// which case out is left as an empty string rather than a truncated fragment,
// since half a JSON document is worse than none.
size_t buildIspindelJson(const IspindelReading& reading, char* out, size_t outLen);
