#include "net.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>

namespace {
// Long enough to type a password on a phone, short enough that a headless
// device left on a dead network gets back to advertising.
constexpr unsigned long kPortalTimeoutSec = 180;
constexpr unsigned long kReconnectIntervalMs = 30000;

String gHostname;
bool gMdnsStarted = false;
unsigned long gLastReconnectMs = 0;

void startMdns() {
  if (gMdnsStarted) {
    return;
  }
  if (!MDNS.begin(gHostname.c_str())) {
    Serial.println("mDNS: start failed");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  gMdnsStarted = true;
  Serial.printf("mDNS: http://%s.local\n", gHostname.c_str());
}
}  // namespace

const String& netHostname() {
  if (gHostname.isEmpty()) {
    // Canonical Arduino-ESP32 chip id: the last three bytes of the MAC, which
    // is what the board prints on its own label.
    const uint64_t mac = ESP.getEfuseMac();
    uint32_t chipId = 0;
    for (int i = 0; i < 17; i += 8) {
      chipId |= ((mac >> (40 - i)) & 0xff) << i;
    }
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "tiltsim-%06x", chipId);
    gHostname = buffer;
  }
  return gHostname;
}

bool netBegin() {
  const String& hostname = netHostname();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname.c_str());

  WiFiManager wm;
  wm.setHostname(hostname.c_str());
  wm.setConfigPortalTimeout(kPortalTimeoutSec);
  wm.setDarkMode(true);

  Serial.printf("WiFi: connecting as %s\n", hostname.c_str());
  if (!wm.autoConnect(hostname.c_str())) {
    Serial.println("WiFi: not connected, continuing offline");
    return false;
  }

  Serial.printf("WiFi: %s  ip=%s  rssi=%d dBm\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  startMdns();
  return true;
}

void netLoop() {
  if (WiFi.status() == WL_CONNECTED) {
    // mDNS cannot start until there is an address, so cover the case where the
    // link came up after boot.
    startMdns();
    return;
  }

  const unsigned long now = millis();
  if (now - gLastReconnectMs < kReconnectIntervalMs) {
    return;
  }
  gLastReconnectMs = now;
  Serial.println("WiFi: link down, reconnecting");
  WiFi.reconnect();
}

bool netIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

String netIpAddress() {
  if (!netIsConnected()) {
    return "0.0.0.0";
  }
  return WiFi.localIP().toString();
}

void netForgetCredentials() {
  Serial.println("WiFi: clearing credentials and restarting");
  WiFiManager wm;
  wm.resetSettings();
  delay(200);
  ESP.restart();
}
