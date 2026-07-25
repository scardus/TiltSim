#include "web_server.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>

#include "net.h"
#include "tilt_config.h"
#include "tilt_encoding.h"
#include "version.h"
#include "web_assets.h"

namespace {
AsyncWebServer gServer(80);

// Handlers below run on the AsyncTCP task, so every touch of gConfig is inside
// configLock()/configUnlock() and persisting is left to loop().

void addTiltJson(JsonObject obj, const size_t index, const TiltSettings& tilt) {
  obj["name"] = kTiltColours[index].name;
  obj["swatch"] = kTiltColours[index].swatch;
  obj["enabled"] = tilt.enabled;
  obj["pro"] = tilt.pro;
  obj["tempF"] = tilt.tempF;
  obj["gravity"] = tilt.gravity;
  obj["tempVarianceF"] = tilt.tempVarianceF;
  obj["gravityVariance"] = tilt.gravityVariance;
}

void sendBusy(AsyncWebServerRequest* request) {
  request->send(503, "text/plain", "Configuration busy, try again");
}

void handleState(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["name"] = FW_NAME;
  doc["version"] = FW_VERSION;
  doc["built"] = FW_BUILD_DATE;
  doc["hostname"] = netHostname();
  doc["ip"] = netIpAddress();
  doc["connected"] = netIsConnected();

  // Which OTA slot is running, and the image hash. An update is only proven to
  // have taken when these change - the build date comes from whichever
  // translation unit last recompiled, so it can stay put across a real update.
  const esp_partition_t* running = esp_ota_get_running_partition();
  doc["partition"] = running != nullptr ? running->label : "?";
  doc["sketchMd5"] = ESP.getSketchMD5();

  if (!configLock()) {
    sendBusy(request);
    return;
  }
  doc["masterEnabled"] = gConfig.masterEnabled;
  const JsonArray tilts = doc["tilts"].to<JsonArray>();
  for (size_t i = 0; i < kTiltCount; ++i) {
    addTiltJson(tilts.add<JsonObject>(), i, gConfig.tilts[i]);
  }
  configUnlock();

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void handleMaster(AsyncWebServerRequest* request, const JsonVariant& body) {
  if (!body["enabled"].is<bool>()) {
    request->send(400, "text/plain", "Expected {\"enabled\": true|false}");
    return;
  }
  if (!configLock()) {
    sendBusy(request);
    return;
  }
  gConfig.masterEnabled = body["enabled"].as<bool>();
  configUnlock();
  configMarkDirty();
  request->send(200, "application/json", "{\"ok\":true}");
}

// Applies a partial patch: only the fields present are touched, so the UI can
// send a single changed field rather than the whole tilt.
void handleTiltPatch(AsyncWebServerRequest* request, const JsonVariant& body) {
  const String path = request->url();
  const int slash = path.lastIndexOf('/');
  const long index = path.substring(slash + 1).toInt();
  if (index < 0 || static_cast<size_t>(index) >= kTiltCount) {
    request->send(404, "text/plain", "No such tilt");
    return;
  }

  if (!configLock()) {
    sendBusy(request);
    return;
  }
  TiltSettings& tilt = gConfig.tilts[index];
  if (body["enabled"].is<bool>()) {
    tilt.enabled = body["enabled"].as<bool>();
  }
  if (body["pro"].is<bool>()) {
    tilt.pro = body["pro"].as<bool>();
  }
  if (body["tempF"].is<float>()) {
    tilt.tempF = body["tempF"].as<float>();
  }
  if (body["gravity"].is<float>()) {
    tilt.gravity = body["gravity"].as<float>();
  }
  if (body["tempVarianceF"].is<float>()) {
    tilt.tempVarianceF = body["tempVarianceF"].as<float>();
  }
  if (body["gravityVariance"].is<float>()) {
    tilt.gravityVariance = body["gravityVariance"].as<float>();
  }
  // Never trust the browser: clamp whatever arrived back into range.
  configClampTilt(tilt);

  JsonDocument doc;
  addTiltJson(doc.to<JsonObject>(), index, tilt);
  configUnlock();
  configMarkDirty();

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

bool gOtaActive = false;
// Tracked separately from Update.hasError(): a file rejected before
// Update.begin() leaves the updater untouched, and reporting that as success
// would tell the user their firmware installed when nothing was written.
bool gOtaStarted = false;
const char* gOtaError = nullptr;

// First byte of every ESP32 firmware image.
constexpr uint8_t kEspImageMagic = 0xE9;

// Long enough for the response to reach the browser before the socket dies.
constexpr unsigned long kRebootGraceMs = 600;

enum class PendingAction { None, Restart, ForgetWifi };
PendingAction gPending = PendingAction::None;
unsigned long gPendingAtMs = 0;

void requestDeferred(const PendingAction action) {
  gPending = action;
  gPendingAtMs = millis();
}

// Multipart upload arrives in chunks on the AsyncTCP task. Update.h streams
// them straight into the inactive OTA slot, so nothing needs buffering.
void handleUploadChunk(AsyncWebServerRequest* request, const String& filename,
                       const size_t index, uint8_t* data, const size_t len,
                       const bool final) {
  if (index == 0) {
    Serial.printf("OTA: receiving %s\n", filename.c_str());
    gOtaStarted = true;
    gOtaError = nullptr;

    // A stray non-firmware file would otherwise be written before anyone
    // noticed it was not an image at all.
    if (len == 0 || data[0] != kEspImageMagic) {
      gOtaError = "Not an ESP32 firmware image";
      Serial.println("OTA: rejected, not an ESP32 firmware image");
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      gOtaError = "Not enough room for the update";
      return;
    }
    gOtaActive = true;
  }

  if (!gOtaActive) {
    return;
  }

  if (Update.write(data, len) != len) {
    Update.printError(Serial);
    gOtaError = "Write failed";
    gOtaActive = false;
    return;
  }

  if (final) {
    if (Update.end(true)) {
      Serial.printf("OTA: wrote %u bytes\n", static_cast<unsigned>(index + len));
    } else {
      Update.printError(Serial);
      gOtaError = "Image rejected by the updater";
    }
    gOtaActive = false;
  }
}

void handleUploadDone(AsyncWebServerRequest* request) {
  if (!gOtaStarted) {
    request->send(400, "text/plain", "No firmware file was uploaded");
    return;
  }
  gOtaStarted = false;

  const bool ok = gOtaError == nullptr && !Update.hasError();
  AsyncWebServerResponse* response = request->beginResponse(
      ok ? 200 : 400, "text/plain",
      ok ? "Update complete" : (gOtaError != nullptr ? gOtaError : "Update failed"));
  // The socket dies with the reboot, so ask the browser not to reuse it.
  response->addHeader("Connection", "close");
  request->send(response);

  if (!ok) {
    return;
  }
  Serial.println("OTA: install complete, rebooting");
  requestDeferred(PendingAction::Restart);
}

void sendProgmem(AsyncWebServerRequest* request, const char* type, const char* body) {
  // PROGMEM is flash-mapped on ESP32, so the plain overload reads it directly.
  AsyncWebServerResponse* response = request->beginResponse(200, type, body);
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}
}  // namespace

bool webOtaInProgress() {
  return gOtaActive;
}

void webServerLoop() {
  if (gPending == PendingAction::None ||
      (millis() - gPendingAtMs < kRebootGraceMs)) {
    return;
  }
  const PendingAction action = gPending;
  gPending = PendingAction::None;

  if (action == PendingAction::ForgetWifi) {
    netForgetCredentials();
    return;
  }
  ESP.restart();
}

void webServerBegin() {
  gServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    sendProgmem(request, "text/html", kIndexHtml);
  });
  gServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* request) {
    sendProgmem(request, "text/css", kStyleCss);
  });
  gServer.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* request) {
    sendProgmem(request, "application/javascript", kAppJs);
  });

  gServer.on("/ota", HTTP_GET, [](AsyncWebServerRequest* request) {
    sendProgmem(request, "text/html", kOtaHtml);
  });
  gServer.on("/update", HTTP_POST, handleUploadDone, handleUploadChunk);

  gServer.on("/api/state", HTTP_GET, handleState);

  gServer.addHandler(new AsyncCallbackJsonWebHandler(
      "/api/master", [](AsyncWebServerRequest* request, JsonVariant& body) {
        handleMaster(request, body);
      }));

  // One handler for every /api/tilt/<n>; the index is parsed back out of the
  // path. The handler keeps a pointer to the URI, so the storage must outlive
  // this function.
  static char paths[kTiltCount][16];
  for (size_t i = 0; i < kTiltCount; ++i) {
    snprintf(paths[i], sizeof(paths[i]), "/api/tilt/%u", static_cast<unsigned>(i));
    gServer.addHandler(new AsyncCallbackJsonWebHandler(
        paths[i], [](AsyncWebServerRequest* request, JsonVariant& body) {
          handleTiltPatch(request, body);
        }));
  }

  gServer.on("/api/reset-wifi", HTTP_POST, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"ok\":true}");
    requestDeferred(PendingAction::ForgetWifi);
  });

  gServer.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Not found");
  });

  gServer.begin();
  Serial.printf("HTTP: http://%s.local/\n", netHostname().c_str());
}
