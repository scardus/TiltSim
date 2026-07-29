#include "net.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_wifi.h>

#include "tilt_encoding.h"

namespace {
// Long enough to type a password on a phone, short enough that a headless
// device left on a dead network gets back to advertising.
constexpr unsigned long kPortalTimeoutSec = 180;
constexpr unsigned long kReconnectIntervalMs = 30000;

// Three attempts of ten seconds, against a default of one attempt of sixty.
// Ten is comfortably longer than a successful associate, which the boot log
// puts at five to nine seconds.
constexpr uint8_t kConnectRetries = 3;
constexpr unsigned long kConnectTimeoutSec = 10;

String gHostname;
// The AP last seen associated, so a change can be reported. Empty until the
// first connection, which is what stops the first pass logging a "move".
String gBssid;
bool gMdnsStarted = false;
bool gPortalSaved = false;
unsigned long gLastReconnectMs = 0;

void startMdns() {
  if (gMdnsStarted) {
    return;
  }
  if (!MDNS.begin(gHostname.c_str())) {
    Serial.println("mDNS: start failed");
    return;
  }
  if (!MDNS.addService("http", "tcp", 80)) {
    // The name still resolves; only the service record is missing, so a browser
    // pointed at the .local name works and only service discovery does not.
    Serial.println("mDNS: http service advertisement failed");
  }
  gMdnsStarted = true;
  Serial.printf("mDNS: http://%s.local\n", gHostname.c_str());
}
}  // namespace

const String& netHostname() {
  if (gHostname.isEmpty()) {
    // The last three bytes of the MAC, which is what the board prints on its own
    // label. Same unpacking the BLE addresses use, so the hostname and the
    // addresses cannot disagree about which board this is.
    BleAddress mac;
    efuseMacBytes(ESP.getEfuseMac(), mac);
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "tiltsim-%02x%02x%02x", mac[3], mac[4],
             mac[5]);
    gHostname = buffer;
  }
  return gHostname;
}

bool netBegin() {
  const String& hostname = netHostname();

  if (!WiFi.mode(WIFI_STA)) {
    Serial.println("WiFi: could not enter station mode");
  }
  if (!WiFi.setHostname(hostname.c_str())) {
    // Cosmetic: the DHCP lease shows up under the default name instead.
    Serial.println("WiFi: hostname not accepted");
  }

  WiFiManager wm;
  wm.setHostname(hostname.c_str());
  wm.setConfigPortalTimeout(kPortalTimeoutSec);
  wm.setDarkMode(true);

  // Three short attempts rather than one long one. The default is a single try
  // with a 60 s timeout, and measured on the bench board that single try fails
  // on about half of all hard resets -- a power cut, in other words. A second
  // attempt costs nothing when the first succeeds, and the whole sequence still
  // gives up sooner than the old single attempt did.
  wm.setConnectRetries(kConnectRetries);
  wm.setConnectTimeout(kConnectTimeoutSec);

  // Do not open the captive portal just because the network was not there at
  // the moment this device happened to boot. Failing to associate is usually a
  // router that is still coming up, not credentials that have gone wrong, and
  // the portal is a poor response to it: it blocks for three minutes, stops the
  // device answering on the real network, and expects somebody to be standing
  // there. netLoop() retries instead, and the portal is kept for the case it is
  // actually for -- no credentials stored at all.
  wm.setEnableConfigPortal(false);
  // Fires only when the portal actually saved a network, so this stays false on
  // the ordinary connect-from-NVS boot.
  wm.setSaveConfigCallback([]() { gPortalSaved = true; });

  Serial.printf("WiFi: connecting as %s\n", hostname.c_str());

  // With the portal disabled above, autoConnect() would answer an unprovisioned
  // device the same way it answers an unreachable router: false, and no way to
  // fix it. Ask first, and raise the portal deliberately for the one case that
  // needs a human. Reading the config is safe -- measured over 18 boots, it has
  // no effect on whether the connect succeeds.
  wifi_config_t stored = {};
  const bool haveCredentials =
      esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK &&
      stored.sta.ssid[0] != '\0';

  if (!haveCredentials) {
    Serial.println("WiFi: no stored network, raising the setup portal");
    if (!wm.startConfigPortal(hostname.c_str())) {
      Serial.println("WiFi: portal closed without a network, continuing offline");
      return false;
    }
  } else if (!wm.autoConnect(hostname.c_str())) {
    // Not fatal any more. The caller starts the web server regardless, and
    // netLoop() keeps retrying, so this recovers by itself when the network
    // comes back rather than needing a reboot nobody is there to give it.
    Serial.println("WiFi: no link yet, retrying in the background");
    return false;
  }

  // The BSSID matters as much as the RSSI here: several APs answer for this
  // SSID, so a weak link is either a near AP fading or an association with a
  // distant one, and the number alone cannot tell those apart.
  Serial.printf("WiFi: %s  bssid=%s  ip=%s  rssi=%d dBm\n", WiFi.SSID().c_str(),
                WiFi.BSSIDstr().c_str(), WiFi.localIP().toString().c_str(),
                WiFi.RSSI());

  // The portal's own web server has been torn down by now, but the browser that
  // submitted the form still has a keep-alive socket to it, and closing that
  // leaves a PCB on port 80 for up to ~80 s (FIN_WAIT_2, then TIME_WAIT).
  // AsyncTCP binds with the raw lwIP API and no SO_REUSEADDR, so the admin
  // server's bind fails with ERR_USE (-8) -- and AsyncServer::begin() returns
  // void, so it fails silently and the UI is simply unreachable. Restarting is
  // cheaper than waiting it out: the next boot connects straight from NVS and
  // never opens port 80 in the first place.
  if (gPortalSaved) {
    Serial.println("WiFi: credentials saved, restarting so port 80 is free");
    Serial.flush();
    delay(200);
    ESP.restart();
  }

  startMdns();
  return true;
}

void netLoop() {
  if (WiFi.status() == WL_CONNECTED) {
    // mDNS cannot start until there is an address, so cover the case where the
    // link came up after boot.
    startMdns();

    // A reconnect can land on a different AP than the one booted onto, and
    // nothing else would say so. Logged only when it changes, so a stable link
    // stays silent.
    const String bssid = WiFi.BSSIDstr();
    if (bssid != gBssid) {
      if (!gBssid.isEmpty()) {
        Serial.printf("WiFi: moved from %s to %s, rssi=%d dBm\n", gBssid.c_str(),
                      bssid.c_str(), WiFi.RSSI());
      }
      gBssid = bssid;
    }
    return;
  }

  const unsigned long now = millis();
  if (now - gLastReconnectMs < kReconnectIntervalMs) {
    return;
  }
  gLastReconnectMs = now;
  Serial.println("WiFi: link down, reconnecting");
  if (!WiFi.reconnect()) {
    // Retried on the next interval regardless; logged so a link that never
    // comes back is distinguishable from one that reconnects and drops again.
    Serial.println("WiFi: reconnect request refused");
  }
}

bool netIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

int netRssi() {
  if (!netIsConnected()) {
    return 0;
  }
  return WiFi.RSSI();
}

String netIpAddress() {
  if (!netIsConnected()) {
    return "0.0.0.0";
  }
  return WiFi.localIP().toString();
}

String netBssid() {
  if (!netIsConnected()) {
    return String();
  }
  return WiFi.BSSIDstr();
}

void netForgetCredentials() {
  Serial.println("WiFi: clearing credentials and restarting");
  WiFiManager wm;
  wm.resetSettings();
  delay(200);
  ESP.restart();
}
