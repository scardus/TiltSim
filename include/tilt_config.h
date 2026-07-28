#pragma once

#include <cstddef>
#include <cstdint>

#include "ispindel_encoding.h"
#include "tilt_encoding.h"

// Runtime configuration, edited from the web UI and persisted to NVS.
//
// The web server's handlers run on the AsyncTCP task, not on loop(), so every
// access goes through configLock()/configUnlock(). Persisting is deferred to
// loop() because NVS writes are slow and must not block the TCP task.

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

// Value limits, enforced server-side on every write.
constexpr float kMinTempF = -40.0f;
constexpr float kMaxTempF = 250.0f;
constexpr float kMinGravity = 0.900f;
constexpr float kMaxGravity = 2.000f;
constexpr float kMaxTempVarianceF = 20.0f;
constexpr float kMaxGravityVariance = 0.100f;

// Loads from NVS, falling back to defaults if absent or written by an
// incompatible build. Creates the config mutex, so call once before any other
// function here.
void configBegin();

// Serial-console dump of the active configuration.
void configPrint();

// Marks the config dirty so loop() flushes it to NVS after a short debounce.
void configMarkDirty();

// Persists to NVS if dirty and the debounce has elapsed. Call from loop().
void configFlushIfDue();

// Persists immediately if dirty, ignoring the debounce.
//
// For use on the way to a restart: the debounce is 1 s but a deferred reboot
// fires after 600 ms, so without this an edit made just before Reboot or Forget
// WiFi was discarded -- after the UI had already said "Saved".
void configFlushNow();

// Clamps one tilt's values into their valid ranges. The caller must hold the
// lock; used by the web handlers after applying a patch.
void configClampTilt(TiltSettings& tilt);

// The same for an iSpindel, which shares the tilts' temperature and gravity
// limits and additionally forces both strings to be NUL terminated.
void configClampIspindel(IspindelSettings& ispindel);

// One draw from a +/-range variance band, in the same real units as the range.
// Shared so the BLE scheduler and the iSpindel poster jitter their readings
// identically rather than growing two subtly different versions.
float randomVariance(float range);

bool configLock();
void configUnlock();

// Only touch this while holding the lock.
extern AppConfig gConfig;
