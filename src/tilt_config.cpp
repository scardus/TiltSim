#include "tilt_config.h"

#include <Arduino.h>
#include <Preferences.h>

AppConfig gConfig;

namespace {
// Bump the low byte whenever the layout of AppConfig changes; a mismatch makes
// the device fall back to defaults rather than read a stale struct.
constexpr uint32_t kConfigMagic = 0x54494C31;  // "TIL1"
constexpr char kPrefsNamespace[] = "tiltsim";
constexpr char kPrefsKey[] = "cfg";

// Long enough that dragging a slider does not hammer the flash, short enough
// that a power cut just after an edit still keeps it.
constexpr unsigned long kSaveDebounceMs = 1000;

SemaphoreHandle_t gConfigMutex = nullptr;
bool gDirty = false;
unsigned long gDirtyAtMs = 0;

float clampFloat(const float value, const float low, const float high) {
  if (!isfinite(value)) {
    return low;
  }
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

void applyDefaults() {
  gConfig.magic = kConfigMagic;
  gConfig.masterEnabled = true;

  // Mirrors the compile-time table this replaces, converted to real units.
  // Black was the hand-scaled "pro" row and now says so explicitly.
  constexpr float kDefaultTempF[kTiltCount] = {64, 66, 68, 69, 71, 73, 75, 77};
  constexpr float kDefaultGravity[kTiltCount] = {
    1.051f, 1.052f, 1.053f, 1.054f, 1.055f, 1.056f, 1.057f, 1.058f};
  constexpr bool kDefaultEnabled[kTiltCount] = {
    true, true, true, false, false, false, false, false};

  for (size_t i = 0; i < kTiltCount; ++i) {
    TiltSettings& tilt = gConfig.tilts[i];
    tilt.enabled = kDefaultEnabled[i];
    tilt.pro = (i == 2);  // Black
    tilt.tempF = kDefaultTempF[i];
    tilt.gravity = kDefaultGravity[i];
    tilt.tempVarianceF = 2.0f;  // matches the old random(-2, 3) swing
    tilt.gravityVariance = 0.0f;
  }
}

void saveNow() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println("Config: NVS open failed, not saved");
    return;
  }
  const size_t written = prefs.putBytes(kPrefsKey, &gConfig, sizeof(gConfig));
  prefs.end();

  if (written != sizeof(gConfig)) {
    Serial.println("Config: short write, not saved");
    return;
  }
  Serial.println("Config: saved");
}
}  // namespace

bool configLock() {
  if (gConfigMutex == nullptr) {
    return false;
  }
  return xSemaphoreTake(gConfigMutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

void configUnlock() {
  if (gConfigMutex != nullptr) {
    xSemaphoreGive(gConfigMutex);
  }
}

void configBegin() {
  gConfigMutex = xSemaphoreCreateMutex();

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    Serial.println("Config: no stored settings, using defaults");
    applyDefaults();
    configMarkDirty();
    return;
  }

  AppConfig stored = {};
  const size_t read = prefs.getBytes(kPrefsKey, &stored, sizeof(stored));
  prefs.end();

  if (read != sizeof(stored) || stored.magic != kConfigMagic) {
    Serial.println("Config: stored settings missing or incompatible, using defaults");
    applyDefaults();
    configMarkDirty();
    return;
  }

  gConfig = stored;
  for (size_t i = 0; i < kTiltCount; ++i) {
    configClampTilt(gConfig.tilts[i]);
  }
  Serial.println("Config: loaded from NVS");
}

void configClampTilt(TiltSettings& tilt) {
  tilt.tempF = clampFloat(tilt.tempF, kMinTempF, kMaxTempF);
  tilt.gravity = clampFloat(tilt.gravity, kMinGravity, kMaxGravity);
  tilt.tempVarianceF = clampFloat(tilt.tempVarianceF, 0.0f, kMaxTempVarianceF);
  tilt.gravityVariance = clampFloat(tilt.gravityVariance, 0.0f, kMaxGravityVariance);
}

void configMarkDirty() {
  gDirty = true;
  gDirtyAtMs = millis();
}

void configFlushIfDue() {
  if (!gDirty || (millis() - gDirtyAtMs < kSaveDebounceMs)) {
    return;
  }
  if (!configLock()) {
    return;
  }
  saveNow();
  gDirty = false;
  configUnlock();
}

void configResetToDefaults() {
  applyDefaults();
  configMarkDirty();
}

void configPrint() {
  Serial.print("Config: master ");
  Serial.println(gConfig.masterEnabled ? "enabled" : "disabled");
  for (size_t i = 0; i < kTiltCount; ++i) {
    const TiltSettings& tilt = gConfig.tilts[i];
    Serial.printf("  %-6s %-8s %-5s %6.1f degF +/-%.1f  %.4f SG +/-%.4f\n",
                  kTiltColours[i].name, tilt.enabled ? "enabled" : "disabled",
                  tilt.pro ? "pro" : "std", tilt.tempF, tilt.tempVarianceF,
                  tilt.gravity, tilt.gravityVariance);
  }
}
