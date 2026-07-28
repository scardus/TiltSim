#pragma once

#include <Arduino.h>

// Derived from the eFuse MAC, so it is stable for the life of the board:
// "tiltsim-a1b2c3".
const String& netHostname();

// Joins the saved network, or raises a captive portal named after the host so
// credentials can be entered. Returns false if it gave up and the device is
// running offline - BLE advertising still works in that state.
bool netBegin();

// Handles reconnects. Call from loop().
void netLoop();

bool netIsConnected();

// Signal strength in dBm, or 0 when there is no link. Kept here so callers do
// not have to pull in WiFi.h just to read it.
int netRssi();
String netIpAddress();

// Which access point the device is actually associated with, as
// "aa:bb:cc:dd:ee:ff", or empty when there is no link.
//
// Worth reporting on a network with more than one AP on the same SSID: RSSI
// alone cannot distinguish a near AP fading from the device having associated
// with a distant one, and those have different fixes.
String netBssid();

// Forgets the saved credentials and reboots into the captive portal.
void netForgetCredentials();
