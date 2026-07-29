#include "web_server.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <lwip/sockets.h>

#include <atomic>
#include <cstring>

#include "heap.h"
#include "ispindel.h"
#include "net.h"
#include "ota_rollback.h"
#include "tilt_config.h"
#include "tilt_encoding.h"
#include "version.h"
#include "web_assets.h"
#include "web_support.h"

namespace {
constexpr uint16_t kHttpPort = 80;
AsyncWebServer gServer(kHttpPort);

// Sized so the full /api/state document fits without the stream buffer having
// to grow mid-write: AsyncResponseStream::write() calls cbuf::resizeAdd(), a
// second and larger allocation plus a copy, on a heap that has little room for
// one. The document is the fixed preamble plus eight tilts, and comes to about
// 1.7 KB with every float serialised at its ugliest. A single tilt is far
// smaller, so patch replies get their own modest size.
constexpr size_t kStateBufferBytes = 2048;
constexpr size_t kPatchBufferBytes = 512;

// Enough for the response stream's buffer plus the response object, its header
// String and whatever the JSON document still holds when the buffer is taken.
// Below this, building the reply is what pushes the heap over the edge.
constexpr size_t kStateHeadroomBytes = 4096;
constexpr size_t kPatchHeadroomBytes = 1536;

// Cap on an incoming JSON body. AsyncCallbackJsonWebHandler defaults to 16384
// and calloc()s the declared Content-Length in one contiguous block from
// handleBody(), before any handler here is called -- so the heap guard below
// never gets a say. The largest real body is a single-field patch, well under
// 100 bytes.
constexpr size_t kMaxJsonBodyBytes = 256;

// The iSpindel slots need more: their URL field alone holds 127 characters, and
// a caller setting several fields at once is entitled to do so. Still two
// orders of magnitude below the library's default.
constexpr size_t kMaxIspindelBodyBytes = 512;

// Refusing a request costs a browser one retry. Attempting one without the room
// to finish it costs a reboot: operator new throws std::bad_alloc, nothing in
// the handler chain catches it, and std::terminate calls abort(). So this trades
// a visible, recoverable failure for an invisible, fatal one.
//
// This is load-bearing, not insurance. An idle device has ~34 KB in one piece,
// but five concurrent connections -- one browser page load -- fragment the heap
// so badly that the largest usable block falls to around 1.4 KB while 54 KB is
// still free in total. Measured: 4 refusals in 50 requests over ten rounds of
// five-way parallel load. Every one of those would otherwise have been a
// reboot.
//
// The real fix is fewer connections per page load, not a bigger heap.
bool haveHeadroom(AsyncWebServerRequest* request, const size_t needed) {
  const size_t largest = largestUsableBlock();
  if (largest >= needed) {
    return true;
  }
  Serial.printf("[HTTP] refused %s: largest usable block %u < %u\n",
                request->url().c_str(), static_cast<unsigned>(largest),
                static_cast<unsigned>(needed));
  AsyncWebServerResponse* response =
      request->beginResponse(503, "text/plain", "Busy, retry");
  response->addHeader("Retry-After", "1");
  request->send(response);
  return false;
}

// Belt and braces. AsyncAbstractResponse::_respond() adds this itself in
// ESPAsyncWebServer 3.11 (WebResponses.cpp:340), which it did not always do --
// the streaming responses used below inherit from it, and when they carried no
// Connection header a browser that opened six at once left six sockets held
// open with nothing to close them. Only AsyncBasicResponse has always set it
// (WebResponses.cpp, both constructors), and the handlers here no longer use
// that class. Stated explicitly so the behaviour does not depend on which
// version of the library is installed.
void finishResponse(AsyncWebServerRequest* request, AsyncWebServerResponse* response) {
  response->addHeader("Connection", "close");
  request->send(response);
}

// The port can still be held when we come to bind: WiFiManager's captive portal
// closes its listening socket but not the browser's keep-alive connection, and
// that lingers in FIN_WAIT_2 then TIME_WAIT for up to ~80 s. netBegin() reboots
// after a portal save to avoid exactly that, so this is a backstop -- but a
// headless device whose admin UI never comes back is bad enough to warrant one.
bool gRoutesRegistered = false;
bool gBound = false;
// Set while the setup portal owns port 80. Suppresses the rebind retry, which
// would otherwise fight the portal for the port every five seconds.
bool gSuspended = false;
bool gBindFailureLogged = false;
unsigned long gLastBindAttemptMs = 0;
constexpr unsigned long kBindRetryMs = 5000;

// Handlers below run on the AsyncTCP task, so every touch of gConfig is inside
// configLock()/configUnlock() and persisting is left to loop().

void addIspindelJson(JsonObject obj, const size_t index,
                     const IspindelSettings& ispindel) {
  // The ID is derived from the board's MAC rather than stored, so it is sent
  // for display only; there is no patch field for it.
  obj["id"] = ispindelIdFor(index);
  obj["name"] = ispindel.name;
  obj["url"] = ispindel.url;
  obj["enabled"] = ispindel.enabled;
  obj["tempF"] = ispindel.tempF;
  obj["gravity"] = ispindel.gravity;
  obj["tempVarianceF"] = ispindel.tempVarianceF;
  obj["gravityVariance"] = ispindel.gravityVariance;
}

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
  if (!haveHeadroom(request, kStateHeadroomBytes)) {
    return;
  }
  JsonDocument doc;
  doc["name"] = FW_NAME;
  doc["version"] = FW_VERSION;
  doc["built"] = FW_BUILD_DATE;
  doc["hostname"] = netHostname();
  doc["ip"] = netIpAddress();
  doc["connected"] = netIsConnected();
  // Which AP, and how well heard. Several APs answer for this SSID, so these
  // two together are what distinguish a fading link from a poorly chosen one.
  doc["bssid"] = netBssid();
  doc["rssi"] = netRssi();

  // Which OTA slot is running, and the image hash. An update is only proven to
  // have taken when these change - the build date comes from whichever
  // translation unit last recompiled, so it can stay put across a real update.
  const esp_partition_t* running = esp_ota_get_running_partition();
  doc["partition"] = running != nullptr ? running->label : "?";
  doc["sketchMd5"] = ESP.getSketchMD5();

  // Whether that slot is still on probation. A freshly installed image reads
  // "pending verify" until it confirms itself and "valid" afterwards, so an
  // update is only safely landed once this says valid -- before that, any
  // restart puts the previous firmware back.
  doc["otaState"] = otaRollbackState();

  // Free heap and the largest single block it can still hand out. The second
  // number is the one that matters: fragmentation is what stops a large
  // allocation, and it is invisible from the total alone. Both are here because
  // a heap problem presented as "the web server keeps crashing", and there was
  // no way to see the real cause from outside the device.
  doc["heapFree"] = ESP.getFreeHeap();
  doc["heapLargestBlock"] = largestUsableBlock();

  // Bytes never used on the AsyncTCP task's stack, at its deepest point since
  // boot. Reported because that stack is taken from the heap and held for the
  // life of the device, on a device whose largest usable block is the number
  // above it: its size is worth knowing, and worth being able to re-check after
  // a library update rather than trusting a measurement taken once.
  doc["tcpStackFree"] = uxTaskGetStackHighWaterMark(nullptr);

  // The same for the iSpindel poster, which takes a much larger stack from the
  // same heap. 0 until it has run a round, and while no endpoint is configured
  // it never starts at all.
  doc["ispindelStackFree"] = ispindelStackFree();

  // Snapshot under the lock, build the JSON outside it. Each insert below can
  // allocate, and doing ~70 of them against a fragmented heap while holding the
  // lock blocked loop()'s buildSchedule() and every other handler for the
  // duration. buildSchedule() already works this way (main.cpp); 168 bytes of
  // stack is a cheap trade for not serialising the device on the allocator.
  AppConfig config;
  if (!configLock()) {
    sendBusy(request);
    return;
  }
  config = gConfig;
  configUnlock();

  doc["masterEnabled"] = config.masterEnabled;
  const JsonArray tilts = doc["tilts"].to<JsonArray>();
  for (size_t i = 0; i < kTiltCount; ++i) {
    addTiltJson(tilts.add<JsonObject>(), i, config.tilts[i]);
  }
  const JsonArray ispindels = doc["ispindels"].to<JsonArray>();
  for (size_t i = 0; i < kIspindelCount; ++i) {
    addIspindelJson(ispindels.add<JsonObject>(), i, config.ispindels[i]);
  }

  // Streamed rather than serialised into a String and handed to send(), which
  // would put this document in memory three times over: the JsonDocument, the
  // String, and the copy AsyncBasicResponse keeps. On a device whose largest
  // free block is a few kilobytes that is the difference between a page refresh
  // working and the server running out of contiguous heap mid-request.
  AsyncResponseStream* response =
      request->beginResponseStream("application/json", kStateBufferBytes);
  serializeJson(doc, *response);
  finishResponse(request, response);
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
  configMarkDirty();
  configUnlock();
  request->send(200, "application/json", "{\"ok\":true}");
}

// Applies a partial patch: only the fields present are touched, so the UI can
// send a single changed field rather than the whole tilt.
//
// The index is passed in rather than parsed back out of the URL. Parsing it cost
// two String allocations per request (url() returns a reference, so assigning it
// to a String copied, and substring() allocated again) and got the wrong answer
// for a path like /api/tilt/0/x: the default URI matcher is prefix-with-slash,
// not exact, so that reaches this handler and "x".toInt() silently yields 0.
void handleTiltPatch(AsyncWebServerRequest* request, const size_t index,
                     const JsonVariant& body) {
  if (index >= kTiltCount) {
    request->send(404, "text/plain", "No such tilt");
    return;
  }
  if (!haveHeadroom(request, kPatchHeadroomBytes)) {
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

  // Copy out, then mark dirty and release. Marking inside the lock is what lets
  // configFlushNow() clear the flag while it holds the lock and still be certain
  // no edit fell between the snapshot and the clear.
  const TiltSettings updated = tilt;
  configMarkDirty();
  configUnlock();

  JsonDocument doc;
  addTiltJson(doc.to<JsonObject>(), index, updated);

  AsyncResponseStream* response =
      request->beginResponseStream("application/json", kPatchBufferBytes);
  serializeJson(doc, *response);
  finishResponse(request, response);
}

void handleIspindelPatch(AsyncWebServerRequest* request, const size_t index,
                         const JsonVariant& body) {
  if (index >= kIspindelCount) {
    request->send(404, "text/plain", "No such iSpindel");
    return;
  }
  if (!haveHeadroom(request, kPatchHeadroomBytes)) {
    return;
  }

  // Validated before the lock is taken, so a bad URL cannot leave a
  // half-applied patch behind.
  if (body["url"].is<const char*>()) {
    const char* problem = urlProblem(body["url"].as<const char*>());
    if (problem != nullptr) {
      request->send(400, "text/plain", problem);
      return;
    }
  }

  if (!configLock()) {
    sendBusy(request);
    return;
  }
  IspindelSettings& ispindel = gConfig.ispindels[index];
  if (body["enabled"].is<bool>()) {
    ispindel.enabled = body["enabled"].as<bool>();
  }
  if (body["name"].is<const char*>()) {
    strlcpy(ispindel.name, body["name"].as<const char*>(), sizeof(ispindel.name));
  }
  if (body["url"].is<const char*>()) {
    strlcpy(ispindel.url, body["url"].as<const char*>(), sizeof(ispindel.url));
  }
  if (body["tempF"].is<float>()) {
    ispindel.tempF = body["tempF"].as<float>();
  }
  if (body["gravity"].is<float>()) {
    ispindel.gravity = body["gravity"].as<float>();
  }
  if (body["tempVarianceF"].is<float>()) {
    ispindel.tempVarianceF = body["tempVarianceF"].as<float>();
  }
  if (body["gravityVariance"].is<float>()) {
    ispindel.gravityVariance = body["gravityVariance"].as<float>();
  }
  // Never trust the browser: clamp whatever arrived back into range.
  configClampIspindel(ispindel);

  const IspindelSettings updated = ispindel;
  configMarkDirty();
  configUnlock();

  // Outside the lock: this can start the posting task, which allocates a stack,
  // and anySlotConfigured() takes the lock itself.
  ispindelNoteConfigChanged();

  JsonDocument doc;
  addIspindelJson(doc.to<JsonObject>(), index, updated);

  AsyncResponseStream* response =
      request->beginResponseStream("application/json", kPatchBufferBytes);
  serializeJson(doc, *response);
  finishResponse(request, response);
}

// Written on the AsyncTCP task, read (and gOtaActive cleared) on the loop task.
// Atomic rather than plain: the accesses happen to be single-word and would
// work anyway, but nothing else here states that they cross a task boundary,
// and this is the state whose races are actually load-bearing.
std::atomic<bool> gOtaActive{false};
// Tracked separately from Update.hasError(): a file rejected before
// Update.begin() leaves the updater untouched, and reporting that as success
// would tell the user their firmware installed when nothing was written.
std::atomic<bool> gOtaStarted{false};
std::atomic<const char*> gOtaError{nullptr};

// When the last upload chunk arrived, so a transfer that dies mid-flight can be
// given up on. Only meaningful while gOtaActive. Fixed-width to match
// otaStalled(), which is explicit about it for portability reasons.
std::atomic<uint32_t> gOtaLastChunkMs{0};

// Serialises access to the global Update object across the two tasks that touch
// it: chunks arrive on the AsyncTCP task, while the stall timeout below runs on
// the loop task. Update is a single global with no locking of its own, so
// aborting from one task while the other is inside write() corrupts its state.
// Held only for the duration of a single Update call, so neither task waits long
// enough to matter -- and in the stall case the AsyncTCP task is not running at
// all, which is the whole point.
SemaphoreHandle_t gUpdateMutex = nullptr;

bool updateLock() {
  return gUpdateMutex != nullptr &&
         xSemaphoreTake(gUpdateMutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

void updateUnlock() {
  if (gUpdateMutex != nullptr) {
    xSemaphoreGive(gUpdateMutex);
  }
}

// Generous next to the gap between chunks on a healthy upload, which is
// milliseconds; this only ever fires on a transfer that has actually stopped.
constexpr unsigned long kOtaStallMs = 15000;

// First byte of every ESP32 firmware image.
constexpr uint8_t kEspImageMagic = 0xE9;

// Long enough for the response to reach the browser before the socket dies.
constexpr unsigned long kRebootGraceMs = 600;

enum class PendingAction { None, Restart, ForgetWifi };
// Requested by a handler on the AsyncTCP task, carried out by loop().
std::atomic<PendingAction> gPending{PendingAction::None};
std::atomic<unsigned long> gPendingAtMs{0};

void requestDeferred(const PendingAction action) {
  gPending = action;
  gPendingAtMs = millis();
}

// Multipart upload arrives in chunks on the AsyncTCP task. Update.h streams
// them straight into the inactive OTA slot, so nothing needs buffering.
void handleUploadChunk(AsyncWebServerRequest* request, const String& filename,
                       const size_t index, uint8_t* data, const size_t len,
                       const bool final) {
  gOtaLastChunkMs = millis();

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
    if (!updateLock()) {
      gOtaError = "Updater busy";
      return;
    }
    const bool began = Update.begin(UPDATE_SIZE_UNKNOWN);
    if (!began) {
      Update.printError(Serial);
    }
    updateUnlock();
    if (!began) {
      gOtaError = "Not enough room for the update";
      return;
    }
    gOtaActive = true;
  }

  if (!gOtaActive) {
    return;
  }

  if (!updateLock()) {
    return;  // The stall timeout has it; it is tearing this transfer down.
  }
  // Re-check under the lock: the loop task may have aborted the transfer while
  // this chunk was waiting, in which case Update no longer has a session open
  // and writing to it would restart one behind everyone's back.
  if (!gOtaActive) {
    updateUnlock();
    return;
  }

  if (Update.write(data, len) != len) {
    Update.printError(Serial);
    updateUnlock();
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
  updateUnlock();
}

void handleUploadDone(AsyncWebServerRequest* request) {
  if (!gOtaStarted) {
    request->send(400, "text/plain", "No firmware file was uploaded");
    return;
  }
  gOtaStarted = false;

  const char* error = gOtaError.load();
  const bool ok = error == nullptr && !Update.hasError();
  AsyncWebServerResponse* response = request->beginResponse(
      ok ? 200 : 400, "text/plain",
      ok ? "Update complete" : (error != nullptr ? error : "Update failed"));
  // The socket dies with the reboot, so ask the browser not to reuse it.
  response->addHeader("Connection", "close");
  request->send(response);

  if (!ok) {
    return;
  }
  Serial.println("OTA: install complete, rebooting");
  requestDeferred(PendingAction::Restart);
}

// Asset, makeAsset() and the chunk filler live in lib/web_support so they can
// be tested; see that header for why the parts exist and why makeAsset() is a
// template.

// Streams an embedded asset straight out of flash, a TCP buffer at a time.
//
// The obvious call, beginResponse(200, type, body), looks like it reads the
// PROGMEM directly -- and it does read it, but AsyncBasicResponse stores the
// body in a String (WebResponses.cpp: `_content = content`), so serving the page
// asks the allocator for one contiguous multi-kilobyte block per request,
// against a largest free block that /api/state now reports. Once that
// allocation fails the request simply hangs with no response, while small
// endpoints like /api/state carry on working -- which is what made it look
// like the server was crashing rather than running out of room. It also got
// steadily worse as the page grew.
//
// The filler copies ~1.4 KB at a time into the buffer the stack already owns,
// so the cost no longer scales with the size of the asset -- which is what makes
// inlining the CSS and JS into the page affordable.
void sendAsset(AsyncWebServerRequest* request, const char* type,
               const Asset& asset) {
  // Cheap next to the JSON handlers -- the body never lands on the heap -- but
  // the response object and its header String still have to be allocated.
  if (!haveHeadroom(request, kPatchHeadroomBytes)) {
    return;
  }
  const Asset* a = &asset;
  AsyncWebServerResponse* response = request->beginResponse(
      type, asset.total, [a](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
        return assetChunk(*a, buffer, maxLen, index);
      });
  response->addHeader("Cache-Control", "no-cache");
  finishResponse(request, response);
}

// AsyncWebServer cannot report a bind failure: AsyncServer::begin() returns void
// and only logs "bind error: -8", so the server can quietly never start while
// everything here looks fine. Probing first is the only way to know.
//
// Deliberately without SO_REUSEADDR. AsyncTCP binds with the raw lwIP TCP API,
// which honours no such option, so an unadorned socket fails under exactly the
// conditions AsyncTCP fails -- setting it here would make the probe pass while
// the real bind still lost. A socket that is bound and closed without ever
// listening leaves nothing behind.
bool portIsFree(const uint16_t port) {
  const int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return true;  // Cannot tell; let the real bind try its luck.
  }
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  const bool isFree =
      lwip_bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  lwip_close(fd);
  return isFree;
}

// Retrying is safe: on a failed bind AsyncTCP closes the pcb and nulls it, so
// the `if (_pcb) return;` guard in AsyncServer::begin() does not block a second
// attempt. Routes are registered once, in webServerBegin().
bool tryBind() {
  if (!portIsFree(kHttpPort)) {
    if (!gBindFailureLogged) {
      Serial.printf("HTTP: port %u still in use, retrying every %lu ms\n",
                    kHttpPort, kBindRetryMs);
      gBindFailureLogged = true;
    }
    return false;
  }

  gServer.begin();
  gBound = true;
  Serial.printf("HTTP: http://%s.local/\n", netHostname().c_str());
  return true;
}
}  // namespace

bool webOtaInProgress() {
  return gOtaActive;
}

bool webServerIsBound() {
  return gBound;
}

void webServerSuspend() {
  if (gSuspended) {
    return;
  }
  gSuspended = true;
  if (gBound) {
    gServer.end();
    gBound = false;
    Serial.println("HTTP: released port 80 for the setup portal");
  }
}

void webServerResume() {
  if (!gSuspended) {
    return;
  }
  gSuspended = false;
  // Not rebound here: the port the portal was using can linger in TIME_WAIT for
  // a while after it closes, and webServerLoop() is already the thing that
  // retries a bind patiently. Let it.
  gLastBindAttemptMs = 0;
  gBindFailureLogged = false;
  Serial.println("HTTP: port 80 free again, rebinding shortly");
}

void webServerLoop() {
  // An upload whose connection dies mid-flight never delivers a final chunk and
  // never reports a write error, so nothing else clears gOtaActive -- and while
  // it is set, loop() stops the radio every pass. Left alone the device stays
  // silent until someone power cycles it, which is a poor way to find out an
  // update failed on a device that is not in the room.
  // Read the timestamp *before* sampling the clock.
  //
  // Written the other way round -- millis() - gOtaLastChunkMs -- this aborted
  // healthy uploads. The two are read on different tasks: loop() would sample
  // millis() as 25253, the AsyncTCP task would stamp the next chunk at 25254
  // before loop() got to read it, and the unsigned subtraction then gave
  // 4294967295 rather than -1. Every full-size upload tripped that within
  // seconds, because a 1.7 MB image delivers chunks every few milliseconds for
  // seventeen seconds and only one of them has to land in that window.
  //
  // Sampling the stamp first means a chunk arriving mid-check can only make the
  // measured silence *older* than reality, by the microseconds between the two
  // reads, which cannot manufacture a 15 second gap. otaStalled() then reads
  // the difference as signed, which keeps a wrapped or future-dated value small
  // and negative instead of enormous.
  const uint32_t lastChunkMs = gOtaLastChunkMs.load();
  const uint32_t nowMs = millis();
  if (gOtaActive && otaStalled(nowMs, lastChunkMs, kOtaStallMs)) {
    // Under the lock, so this cannot land in the middle of an Update.write() on
    // the AsyncTCP task. On a genuinely stalled transfer that task is idle and
    // the take is uncontended; if a late chunk is being written right now, this
    // waits for it and that chunk's own re-check then sees gOtaActive false.
    if (updateLock()) {
      Update.abort();
      gOtaActive = false;
      gOtaStarted = false;
      gOtaError = "Upload stalled";
      updateUnlock();
      Serial.printf("OTA: upload stalled for %ld ms, aborted -- advertising resumes\n",
                    static_cast<long>(static_cast<int32_t>(nowMs - lastChunkMs)));
    }
  }

  // gRoutesRegistered guards against binding a server with no handlers on it:
  // an offline boot never calls webServerBegin() at all.
  if (gRoutesRegistered && !gBound && !gSuspended &&
      millis() - gLastBindAttemptMs >= kBindRetryMs) {
    gLastBindAttemptMs = millis();
    if (tryBind()) {
      Serial.println("HTTP: bound on retry");
    }
  }

  if (gPending == PendingAction::None ||
      (millis() - gPendingAtMs < kRebootGraceMs)) {
    return;
  }
  const PendingAction action = gPending;
  gPending = PendingAction::None;

  // The reboot grace is 600 ms but the config debounce is 1 s, so an edit made
  // just before Reboot or Forget WiFi was pressed has not been written yet.
  // Without this it is discarded -- after the UI has already said "Saved".
  configFlushNow();

  if (action == PendingAction::ForgetWifi) {
    netForgetCredentials();
    return;
  }
  ESP.restart();
}

bool webServerBegin() {
  gUpdateMutex = xSemaphoreCreateMutex();

  // Both pages carry their CSS and JS inline, so a page load is two connections
  // (the page and /api/state) rather than five. Five concurrent connections is
  // what fragmented the heap far enough to start refusing requests, and there
  // are no separate /style.css or /app.js routes any more because nothing asks
  // for them -- the favicon link in each page is a data: URI for the same
  // reason, since a 404 still costs a whole connection to deliver.
  static const Asset indexPage =
      makeAsset(kIndexHead, kStyleCss, kIndexMid, kAppJs, kIndexTail);
  static const Asset otaPage = makeAsset(kOtaHead, kStyleCss, kOtaTail);

  gServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    sendAsset(request, "text/html", indexPage);
  });

  gServer.on("/ota", HTTP_GET, [](AsyncWebServerRequest* request) {
    sendAsset(request, "text/html", otaPage);
  });
  gServer.on("/update", HTTP_POST, handleUploadDone, handleUploadChunk);

  gServer.on("/api/state", HTTP_GET, handleState);

  auto* master = new AsyncCallbackJsonWebHandler(
      "/api/master", [](AsyncWebServerRequest* request, JsonVariant& body) {
        handleMaster(request, body);
      });
  // The default is 16 KB, and the library calloc()s exactly that much in one
  // contiguous block before any handler here runs -- so haveHeadroom() cannot
  // see it, let alone refuse it. The real bodies are a few dozen bytes.
  master->setMaxContentLength(kMaxJsonBodyBytes);
  gServer.addHandler(master);

  // One handler per /api/tilt/<n>, each capturing its own index. Capturing is
  // what removes the URL parsing: AsyncCallbackJsonWebHandler takes a
  // std::function, so the lambda does not have to be capture-free.
  for (size_t i = 0; i < kTiltCount; ++i) {
    // The handler keeps a pointer to the URI string, so the storage has to
    // outlive this function.
    static char paths[kTiltCount][16];
    snprintf(paths[i], sizeof(paths[i]), "/api/tilt/%u", static_cast<unsigned>(i));
    auto* handler = new AsyncCallbackJsonWebHandler(
        paths[i], [i](AsyncWebServerRequest* request, JsonVariant& body) {
          handleTiltPatch(request, i, body);
        });
    handler->setMaxContentLength(kMaxJsonBodyBytes);
    gServer.addHandler(handler);
  }

  // The same shape for the iSpindel slots, with a larger cap: a single-field
  // patch here can carry a 127-character URL, where the largest tilt patch is a
  // float.
  for (size_t i = 0; i < kIspindelCount; ++i) {
    static char ispindelPaths[kIspindelCount][20];
    snprintf(ispindelPaths[i], sizeof(ispindelPaths[i]), "/api/ispindel/%u",
             static_cast<unsigned>(i));
    auto* handler = new AsyncCallbackJsonWebHandler(
        ispindelPaths[i], [i](AsyncWebServerRequest* request, JsonVariant& body) {
          handleIspindelPatch(request, i, body);
        });
    handler->setMaxContentLength(kMaxIspindelBodyBytes);
    gServer.addHandler(handler);
  }

  gServer.on("/api/reset-wifi", HTTP_POST, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"ok\":true}");
    requestDeferred(PendingAction::ForgetWifi);
  });

  // Deferred like the others: restarting from a handler starves the AsyncTCP
  // watchdog and panics before the response can flush, so the browser would
  // never learn the reboot was accepted.
  gServer.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"ok\":true}");
    requestDeferred(PendingAction::Restart);
  });

  gServer.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Not found");
  });

  // Failing here is not fatal: webServerLoop() keeps trying, so the UI comes
  // back on its own once whatever is holding the port lets go.
  gRoutesRegistered = true;
  gLastBindAttemptMs = millis();
  return tryBind();
}
