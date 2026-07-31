#pragma once

#include <cstddef>
#include <cstdint>

#include "ispindel_encoding.h"
#include "tilt_encoding.h"

// The shape of what is persisted, and everything that validates a value before
// it is allowed into it.
//
// Arduino-free and separate from tilt_config.h so the layout can be pinned by a
// test. The lifecycle around it -- NVS, the mutex, the write debounce -- stays
// in src/tilt_config.cpp, because none of that is testable off the target and
// none of it is where the dangerous mistakes are.
//
// kTiltCount and kIspindelCount come from the two encoding libraries rather
// than being redeclared here, so the persisted array sizes cannot drift from
// the number of devices actually simulated.

struct TiltSettings {
  bool enabled;
  bool pro;
  float tempF;            // real degrees F, e.g. 68.5
  float gravity;          // real specific gravity, e.g. 1.0530
  float tempVarianceF;    // +/- swing in real degF, 0 disables jitter
  float gravityVariance;  // +/- swing in real SG, 0 disables jitter
};

// Long enough for a descriptive name and a URL with a path and a query string,
// short enough that four of them are a rounding error in the NVS blob.
constexpr size_t kIspindelNameLen = 32;
constexpr size_t kIspindelUrlLen = 128;

// Fixed-size arrays rather than String, because the whole struct is written to
// NVS as one blob and a heap pointer would not survive the round trip.
struct IspindelSettings {
  bool enabled;
  bool extended;   // send the extended Gravitymon field set instead of plain iSpindel
  bool plato;      // gravity-unit is Plato instead of SG; only honoured when extended
  char name[kIspindelNameLen];
  char url[kIspindelUrlLen];
  float tempF;            // real degrees F, as for the tilts
  float gravity;          // real specific gravity
  float tempVarianceF;    // +/- swing in real degF, 0 disables jitter
  float gravityVariance;  // +/- swing in real SG, 0 disables jitter
};

struct AppConfig {
  uint32_t magic;
  bool masterEnabled;
  TiltSettings tilts[kTiltCount];
  IspindelSettings ispindels[kIspindelCount];
};

// Bump the low byte whenever the layout of AppConfig changes; a mismatch makes
// the device fall back to defaults rather than read a stale struct.
// TIL2 added the iSpindel slots. TIL3 added extended/plato to IspindelSettings,
// which left sizeof(IspindelSettings) unchanged (padding absorbed the two new
// bools) but moved name/url -- exactly the case the paragraph below warns about.
//
// configBegin() also checks the stored blob's size, which catches a field being
// added or removed. What it cannot catch is a change that leaves sizeof alone --
// two floats swapped, or a bool moved -- because the magic still matches and the
// length still matches, so a stale blob is read straight into the new layout and
// every value silently belongs to a different field. test_config_schema pins the
// offsets against this constant for that reason: change one, and it tells you to
// change the other.
constexpr uint32_t kConfigMagic = 0x54494C33;  // "TIL3"

// Value limits, enforced server-side on every write.
constexpr float kMinTempF = -40.0f;
constexpr float kMaxTempF = 250.0f;
constexpr float kMinGravity = 0.900f;
constexpr float kMaxGravity = 2.000f;
constexpr float kMaxTempVarianceF = 20.0f;
constexpr float kMaxGravityVariance = 0.100f;

// Clamps one tilt's values into their valid ranges. The caller must hold the
// config lock; used by the web handlers after applying a patch.
void configClampTilt(TiltSettings& tilt);

// The same for an iSpindel, which shares the tilts' temperature and gravity
// limits and additionally forces both strings to be NUL terminated.
void configClampIspindel(IspindelSettings& ispindel);

// Why a URL is unacceptable, or nullptr if it is fine.
//
// Rejected rather than clamped, unlike the numeric fields: there is no sensible
// nearest-valid-URL to snap to, and silently dropping the edit would leave the
// box showing something the device is not using.
const char* urlProblem(const char* url);
