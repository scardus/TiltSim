#include "ispindel.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <atomic>
#include <cstring>

#include "ispindel_encoding.h"
#include "net.h"
#include "tilt_config.h"
#include "tilt_encoding.h"

namespace {
// Matches the interval advertised in the payload. A receiver that spaces its
// own expectations off that value would mark the device late otherwise.
constexpr unsigned long kPostIntervalMs = kIspindelIntervalSec * 1000UL;

// The first round fires well before the first full interval so a freshly typed
// URL can be checked without waiting a quarter of an hour. Long enough after
// boot that WiFi and DNS have settled.
constexpr unsigned long kFirstPostDelayMs = 30000;

// A black-hole endpoint must not wedge the task until the next interval.
constexpr uint16_t kHttpTimeoutMs = 10000;

// A TLS handshake is the deepest thing this task does. Measured with
// uxTaskGetStackHighWaterMark(), reported by /api/state as ispindelStackFree so
// it can be re-checked rather than taken on trust: a plain HTTP round peaked at
// 3448 bytes and an https one at 5000, against webhook.site.
//
// 16384 leaves 11384 spare, about 3.3x the worst case seen -- in line with the
// 2.9x CONFIG_ASYNC_TCP_STACK_SIZE carries. Kept at 3.3x rather than trimmed
// because TLS stack depth varies with the peer's certificate chain and cipher,
// and one endpoint is one data point. There is room for it: the heap has ~94 KB
// in one piece with this task running.
constexpr uint32_t kTaskStackBytes = 16384;

// Below the Arduino loop task (priority 1), so the BLE rotation always wins.
constexpr UBaseType_t kTaskPriority = 1;

// Room for the longest body this can produce: a full-length name escaped
// character by character, plus the fixed fields.
constexpr size_t kBodyLen = 384;

// Deliberately not a static stack in .bss. That looked free -- there is plenty
// of static RAM spare -- but .bss and the heap come out of the same DRAM, so an
// array here is memory the web server never gets, spent whether or not a single
// iSpindel is configured. An earlier version of this file reserved 16 KB that
// way on a device whose whole heap was around 32 KB, which halved it
// permanently for a feature nobody had switched on, and cost more in
// fragmentation than in bytes: the array sat where it split the region
// responses are assembled in.
//
// Taking the stack from the heap is safe because ispindelBegin() runs before
// NimBLEDevice::init(): WiFi is up but the BLE stack is not, and that is the
// roomiest the heap ever is.
TaskHandle_t gTaskHandle = nullptr;

// Written by the task, read by the web handler on the AsyncTCP task.
std::atomic<uint32_t> gStackFree{0};

char gIds[kIspindelCount][kIspindelIdLen];

// Sends one already-built body over a caller-chosen client.
//
// The client is a parameter rather than a local so that only the one actually
// needed is ever constructed. WiFiClientSecure carries an mbedTLS context, and
// declaring both here would build it for every plain-HTTP post as well.
// body is not const because HTTPClient::POST() takes a mutable pointer; it does
// not modify the buffer, but there is no const overload to hand it to.
void sendBody(WiFiClient& client, const IspindelSettings& settings, char* body,
              const float tempF, const float gravity) {
  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  // Endpoints that redirect are common enough to be worth following, and the
  // body is idempotent so replaying it is harmless.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, settings.url)) {
    Serial.printf("iSpindel: %s -> %s unusable URL\n", settings.name,
                  settings.url);
    return;
  }
  http.addHeader("Content-Type", "application/json");

  const int status = http.POST(reinterpret_cast<uint8_t*>(body), strlen(body));
  if (status > 0) {
    Serial.printf("iSpindel: %s -> %s %d  %.1f degF  %.4f SG\n", settings.name,
                  settings.url, status, tempF, gravity);
  } else {
    // Negative codes are HTTPClient's own, not the server's; the string form is
    // the only way to tell a refused connection from a timeout.
    Serial.printf("iSpindel: %s -> %s failed: %s\n", settings.name, settings.url,
                  http.errorToString(status).c_str());
  }
  http.end();
}

// Posts one reading. The reference is to the caller's own copy, taken out from
// under the config lock in postRound(), so it stays valid for the whole request
// without keeping gConfig pinned.
void postOne(const size_t index, const IspindelSettings& settings) {
  const float tempF = settings.tempF + randomVariance(settings.tempVarianceF);
  const float gravity =
      settings.gravity + randomVariance(settings.gravityVariance);

  char body[kBodyLen];
  const IspindelReading reading = {settings.name, gIds[index], tempF, gravity};
  if (buildIspindelJson(reading, body, sizeof(body)) == 0) {
    Serial.printf("iSpindel: %s payload would not fit, skipped\n", settings.name);
    return;
  }

  if (strncmp(settings.url, "https://", 8) == 0) {
    WiFiClientSecure tls;
    // No certificate validation. Real iSpindel and Gravitymon firmware does the
    // same -- there is nowhere to keep a trust store and no clock to check
    // validity against. It means a machine on the path could read or alter the
    // readings, which for simulated brew data is a fair trade for being able to
    // post to a cloud endpoint at all.
    tls.setInsecure();
    tls.setTimeout(kHttpTimeoutMs / 1000);
    sendBody(tls, settings, body, tempF, gravity);
  } else {
    WiFiClient plain;
    sendBody(plain, settings, body, tempF, gravity);
  }
}

void postRound() {
  // One snapshot for the whole round rather than one per slot, so a round
  // cannot mix settings from before and after an edit.
  if (!configLock()) {
    Serial.println("iSpindel: config busy, skipping this round");
    return;
  }
  const bool masterEnabled = gConfig.masterEnabled;
  IspindelSettings slots[kIspindelCount];
  for (size_t i = 0; i < kIspindelCount; ++i) {
    slots[i] = gConfig.ispindels[i];
  }
  configUnlock();

  if (!masterEnabled) {
    return;
  }

  for (size_t i = 0; i < kIspindelCount; ++i) {
    if (!slots[i].enabled || slots[i].url[0] == '\0') {
      continue;
    }
    // Checked through net.h rather than WiFi.status() so this file does not
    // pull in WiFi.h for one enum.
    if (!netIsConnected()) {
      Serial.println("iSpindel: link down, skipping this round");
      return;
    }
    postOne(i, slots[i]);
  }

  gStackFree = uxTaskGetStackHighWaterMark(nullptr);
}

// A slot with no URL can never post, so it does not justify the stack.
bool anySlotConfigured() {
  if (!configLock()) {
    return true;  // Cannot tell; assume yes rather than silently disable it.
  }
  bool configured = false;
  for (size_t i = 0; i < kIspindelCount; ++i) {
    if (gConfig.ispindels[i].url[0] != '\0') {
      configured = true;
      break;
    }
  }
  configUnlock();
  return configured;
}

void ispindelTask(void*) {
  // Notification rather than vTaskDelay so ispindelNoteConfigChanged() can cut
  // the wait short. Waiting out a quarter of an hour to find out whether a URL
  // typed thirty seconds ago works is the difference between a testable feature
  // and one nobody bothers to configure.
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kFirstPostDelayMs));
  for (;;) {
    postRound();
    // Waiting after the round rather than tracking a deadline means a slow
    // round pushes the next one out. That is the honest behaviour for a device
    // that sleeps between readings, and it cannot pile rounds up on top of each
    // other when an endpoint is timing out.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPostIntervalMs));
  }
}

bool startTask() {
  const uint32_t before = ESP.getFreeHeap();
  if (xTaskCreate(ispindelTask, "ispindel", kTaskStackBytes, nullptr,
                  kTaskPriority, &gTaskHandle) != pdPASS) {
    gTaskHandle = nullptr;
    Serial.println("iSpindel: not enough heap for the posting task");
    return false;
  }
  Serial.printf("iSpindel: task started, %u bytes of heap for its stack\n",
                static_cast<unsigned>(before - ESP.getFreeHeap()));
  return true;
}
}  // namespace

void ispindelBegin() {
  // The same unpacking the hostname and the BLE addresses use, rather than a
  // third route to the MAC: efuseMacBytes() exists so those cannot drift apart.
  BleAddress baseMac;
  efuseMacBytes(ESP.getEfuseMac(), baseMac);

  for (size_t i = 0; i < kIspindelCount; ++i) {
    if (!ispindelId(baseMac.data(), i, gIds[i], kIspindelIdLen)) {
      gIds[i][0] = '\0';
      Serial.printf("iSpindel: could not derive an ID for slot %u\n",
                    static_cast<unsigned>(i));
    }
  }

  Serial.printf("iSpindel: %u slots, posting every %lu s, IDs",
                static_cast<unsigned>(kIspindelCount), kIspindelIntervalSec);
  for (size_t i = 0; i < kIspindelCount; ++i) {
    Serial.printf(" %s", gIds[i]);
  }
  Serial.println();

  // No endpoint anywhere means the task would wake every quarter of an hour,
  // find nothing to do and go back to sleep, having held a stack for the
  // privilege. A URL saved later starts it without a reboot.
  if (!anySlotConfigured()) {
    Serial.println("iSpindel: no endpoints configured, task not started");
    return;
  }
  startTask();
}

void ispindelNoteConfigChanged() {
  if (gTaskHandle != nullptr) {
    // Already running: wake it so the new settings are posted now.
    xTaskNotifyGive(gTaskHandle);
    return;
  }
  if (!anySlotConfigured()) {
    return;
  }
  // Started late, so this is the one path that has to find a stack's worth of
  // contiguous heap with BLE and WiFi already up. It reports plainly if it
  // cannot, and a reboot will start it from the roomier path in setup().
  if (!startTask()) {
    Serial.println("iSpindel: reboot to start posting");
  }
}

const char* ispindelIdFor(const size_t index) {
  if (index >= kIspindelCount) {
    return "";
  }
  return gIds[index];
}

uint32_t ispindelStackFree() {
  return gStackFree.load();
}
