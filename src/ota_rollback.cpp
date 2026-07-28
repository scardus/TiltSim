#include "ota_rollback.h"

#include <Arduino.h>
#include <esp_ota_ops.h>

#include "net.h"
#include "tilt_config.h"
#include "web_server.h"

// Takes charge of confirming a freshly installed image.
//
// The bootloader shipped with the Arduino core is built with
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, so the machinery for this already
// exists and is already running: Update.end() calls
// esp_ota_set_boot_partition(), which marks the new slot ESP_OTA_IMG_NEW, and
// the bootloader promotes that to ESP_OTA_IMG_PENDING_VERIFY as it boots the
// image. If it ever sees PENDING_VERIFY again -- that is, the image booted a
// second time without anyone confirming the first -- it marks the slot ABORTED
// and boots the other one instead.
//
// What was missing is the confirmation being worth anything. initArduino()
// calls esp_ota_mark_app_valid_cancel_rollback() before setup() runs
// (esp32-hal-misc.c:229), which confirms every image that manages to reach main
// -- including one that panics in setup() a millisecond later. Returning true
// from this weak hook suppresses that, leaving the confirmation to
// otaRollbackLoop() below once the device has actually done its job for a while.
//
// extern "C" because the weak definition being overridden is in a .c file; a
// C++ one would mangle and quietly fail to override it, and the failure looks
// exactly like everything working.
extern "C" bool verifyRollbackLater() {
  return true;
}

namespace {
// How long the image has to run before it counts as good. Long enough to be
// past setup(), the first BLE cycles and the first web requests; short enough
// that the window where an unlucky power cut costs the update stays small.
constexpr unsigned long kConfirmUptimeMs = 60000;

// And how long to wait for the health check below before concluding the image
// is broken and going back. Generous: a slow DHCP lease, a long captive portal
// timeout and a port 80 bind that has to wait out a lingering socket all
// resolve well inside it.
constexpr unsigned long kGiveUpMs = 300000;

// Set when the running image is on probation. Everything here is a no-op
// otherwise, which is the usual case -- a USB flash leaves the slot UNDEFINED
// and only an OTA install starts a probation.
bool gPending = false;

// Confirmed, rolled back, or nothing to do: stop looking either way.
bool gSettled = false;

const char* stateName(const esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:      return "undefined";
    default:                         return "unknown";
  }
}

bool runningState(esp_ota_img_states_t& state) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  return running != nullptr &&
         esp_ota_get_state_partition(running, &state) == ESP_OK;
}

// What "working" means for this device, and the one judgement call in here.
//
// Not simply "WiFi is up": a device carried somewhere without its network is
// still doing the job it exists to do, and rolling its firmware back for that
// would be wrong. So an offline device passes -- it is advertising, which is
// the whole point of it, and the update is not what took the network away.
//
// The failure actually worth catching is the one that leaves a headless device
// needing physical access: it joins the network but the admin UI never comes
// back, so there is no way in to fix it. Hence the second half -- if there is a
// link, port 80 has to be answering on it.
bool looksHealthy() {
  return !netIsConnected() || webServerIsBound();
}

void confirm(const unsigned long uptimeMs) {
  const esp_err_t rc = esp_ota_mark_app_valid_cancel_rollback();
  gSettled = true;
  if (rc == ESP_OK) {
    Serial.printf("Rollback: image confirmed good after %lu s, this slot is now permanent\n",
                  uptimeMs / 1000);
    return;
  }
  // The image is fine but says so nowhere on flash, so the next restart rolls
  // it back. That is the safe direction -- the device comes up on the previous
  // firmware rather than on nothing -- but it is not what was asked for, and
  // silently losing an update at the next power cut would be baffling.
  Serial.printf("Rollback: could not record the confirmation (rc=%d) -- "
                "the next restart will go back to the previous firmware\n", rc);
}

void giveUp(const unsigned long uptimeMs) {
  gSettled = true;
  // The other slot can be empty (a board only ever flashed over USB) or hold an
  // image the bootloader has already rejected. Rolling back to nothing is worse
  // than staying on a half-working image, which at least still advertises.
  if (!esp_ota_check_rollback_is_possible()) {
    Serial.println("Rollback: no working image to fall back to, staying on this one");
    return;
  }

  Serial.printf("Rollback: still unhealthy after %lu s, going back to the previous firmware\n",
                uptimeMs / 1000);
  // Same reasoning as the deferred reboot in webServerLoop(): the config
  // debounce is a second long, and a restart is not a reason to drop an edit.
  configFlushNow();
  Serial.flush();

  esp_ota_mark_app_invalid_rollback_and_reboot();
  // Only reached if the fallback was refused after all -- it does not return
  // otherwise.
  Serial.println("Rollback: the fallback was refused, staying on this image");
}
}  // namespace

void otaRollbackBegin() {
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (!runningState(state)) {
    // Nothing can be confirmed or rolled back without it, so say so rather than
    // leaving a device that silently has no safety net.
    Serial.println("Boot: OTA state unreadable, rollback protection is off");
    gSettled = true;
    return;
  }

  // Which slot was last rejected, if any. Not necessarily this boot -- the
  // record persists -- but after a rollback this is the image that failed, and
  // it is the only trace of it left.
  const esp_partition_t* rejected = esp_ota_get_last_invalid_partition();
  Serial.printf("Boot: running %s (%s), last rejected %s\n",
                esp_ota_get_running_partition()->label, stateName(state),
                rejected != nullptr ? rejected->label : "none");

  gPending = state == ESP_OTA_IMG_PENDING_VERIFY;
  gSettled = !gPending;
  if (gPending) {
    Serial.printf("Rollback: this image is on probation, confirming after %lu s of health\n",
                  kConfirmUptimeMs / 1000);
  }
}

void otaRollbackLoop() {
  if (gSettled) {
    return;
  }
  // An upload is the one thing that must not be interrupted here: rebooting
  // mid-transfer wastes the one route someone has to fix a broken image, and it
  // is exactly when they would be using it.
  if (webOtaInProgress()) {
    return;
  }

  const unsigned long uptimeMs = millis();
  if (looksHealthy()) {
    if (uptimeMs >= kConfirmUptimeMs) {
      confirm(uptimeMs);
    }
    return;
  }
  if (uptimeMs >= kGiveUpMs) {
    giveUp(uptimeMs);
  }
}

const char* otaRollbackState() {
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (!runningState(state)) {
    return "unreadable";
  }
  return stateName(state);
}
