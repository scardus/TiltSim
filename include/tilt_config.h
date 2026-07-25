#pragma once

#include <cstddef>
#include <cstdint>

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

struct AppConfig {
  uint32_t magic;
  bool masterEnabled;
  TiltSettings tilts[kTiltCount];
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

// Restores built-in defaults and marks dirty.
void configResetToDefaults();

// Clamps one tilt's values into their valid ranges. The caller must hold the
// lock; used by the web handlers after applying a patch.
void configClampTilt(TiltSettings& tilt);

bool configLock();
void configUnlock();

// Only touch this while holding the lock.
extern AppConfig gConfig;
