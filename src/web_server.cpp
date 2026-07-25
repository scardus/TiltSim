#include "web_server.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

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

void sendProgmem(AsyncWebServerRequest* request, const char* type, const char* body) {
  AsyncWebServerResponse* response = request->beginResponse_P(200, type, body);
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}
}  // namespace

bool webOtaInProgress() {
  return false;
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
    // Let the response flush before the reboot takes the socket down.
    delay(200);
    netForgetCredentials();
  });

  gServer.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Not found");
  });

  gServer.begin();
  Serial.printf("HTTP: http://%s.local/\n", netHostname().c_str());
}
