#include "tilt_config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_rom_crc.h>

#include <cstring>

AppConfig gConfig;

namespace {
// kConfigMagic lives in config_schema.h, next to the layout it describes.
constexpr char kPrefsNamespace[] = "tiltsim";
constexpr char kPrefsKey[] = "cfg";

// Long enough that dragging a slider does not hammer the flash, short enough
// that a power cut just after an edit still keeps it.
constexpr unsigned long kSaveDebounceMs = 1000;

SemaphoreHandle_t gConfigMutex = nullptr;
bool gDirty = false;
unsigned long gDirtyAtMs = 0;

// CRC of the blob last written, so an edit that changes nothing does not cost a
// flash write. A CRC rather than a second AppConfig because this lives in .bss
// for the life of the device and 4 bytes is not 168.
uint32_t gSavedCrc = 0;
bool gHaveSavedCrc = false;

uint32_t configCrc(const AppConfig& config) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&config),
                          sizeof(config));
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

  // Off with no endpoint, because an iSpindel with nowhere to post is the only
  // safe default: enabling one by default would have the device POST to
  // whatever the previous owner of that URL is now serving.
  constexpr float kDefaultIspindelTempF[kIspindelCount] = {64, 66, 68, 70};
  constexpr float kDefaultIspindelGravity[kIspindelCount] = {1.050f, 1.051f,
                                                             1.052f, 1.053f};

  for (size_t i = 0; i < kIspindelCount; ++i) {
    IspindelSettings& ispindel = gConfig.ispindels[i];
    ispindel.enabled = false;
    ispindel.extended = false;
    ispindel.plato = false;
    snprintf(ispindel.name, sizeof(ispindel.name), "ispindel-%u",
             static_cast<unsigned>(i + 1));
    ispindel.url[0] = '\0';
    ispindel.tempF = kDefaultIspindelTempF[i];
    ispindel.gravity = kDefaultIspindelGravity[i];
    ispindel.tempVarianceF = 2.0f;
    ispindel.gravityVariance = 0.0f;
  }
}

// Writes a snapshot rather than reading gConfig, so the caller can release the
// config lock first: an NVS commit is tens of ms and the lock has a 100 ms
// timeout, so holding it across the write turned a slider drag during a flush
// into a 503 for the browser.
//
// Returns false only when the write was attempted and failed. A no-op skip
// counts as success -- there is nothing left to retry.
bool saveNow(const AppConfig& config) {
  const uint32_t crc = configCrc(config);
  if (gHaveSavedCrc && crc == gSavedCrc) {
    return true;  // Byte-identical to what is already stored.
  }

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println("Config: NVS open failed, not saved");
    return false;
  }
  const size_t written = prefs.putBytes(kPrefsKey, &config, sizeof(config));
  prefs.end();

  if (written != sizeof(config)) {
    Serial.println("Config: short write, not saved");
    return false;
  }
  gSavedCrc = crc;
  gHaveSavedCrc = true;
  Serial.println("Config: saved");
  return true;
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

  // Record what NVS actually holds, so a session that changes nothing -- or
  // changes a value and puts it back -- costs no flash write at all.
  gSavedCrc = configCrc(stored);
  gHaveSavedCrc = true;

  gConfig = stored;
  for (size_t i = 0; i < kTiltCount; ++i) {
    configClampTilt(gConfig.tilts[i]);
  }
  for (size_t i = 0; i < kIspindelCount; ++i) {
    configClampIspindel(gConfig.ispindels[i]);
  }
  Serial.println("Config: loaded from NVS");
}

float randomVariance(const float range) {
  if (range <= 0.0f) {
    return 0.0f;
  }
  // random() is integer-only, so work in thousandths of the range.
  const long steps = random(-1000, 1001);
  return range * (static_cast<float>(steps) / 1000.0f);
}

void configMarkDirty() {
  gDirty = true;
  gDirtyAtMs = millis();
}

void configFlushIfDue() {
  if (!gDirty || (millis() - gDirtyAtMs < kSaveDebounceMs)) {
    return;
  }
  configFlushNow();
}

void configFlushNow() {
  if (!gDirty) {
    return;
  }
  if (!configLock()) {
    return;  // Next pass; still dirty, so nothing is lost.
  }
  // Snapshot and clear inside the lock, write outside it. Clearing here is what
  // makes a concurrent edit safe: a handler marks dirty again while still
  // holding the lock, so its change is either already in this snapshot or will
  // be caught by the next flush. It can never fall between the two.
  const AppConfig snapshot = gConfig;
  gDirty = false;
  configUnlock();

  if (!saveNow(snapshot)) {
    gDirty = true;
    gDirtyAtMs = millis();
  }
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
  for (size_t i = 0; i < kIspindelCount; ++i) {
    const IspindelSettings& ispindel = gConfig.ispindels[i];
    Serial.printf("  %-12s %-8s %6.1f degF +/-%.1f  %.4f SG +/-%.4f  %s\n",
                  ispindel.name, ispindel.enabled ? "enabled" : "disabled",
                  ispindel.tempF, ispindel.tempVarianceF, ispindel.gravity,
                  ispindel.gravityVariance,
                  ispindel.url[0] != '\0' ? ispindel.url : "(no endpoint)");
  }
}
