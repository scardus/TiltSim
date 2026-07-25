#pragma once

// Starts the HTTP server on port 80. Call after the network is up.
void webServerBegin();

// Carries out reboots requested by a handler. Call from loop().
//
// Handlers run on the AsyncTCP task, which is watchdogged: calling delay() or
// ESP.restart() from one starves the watchdog and panics the device before the
// response can flush. So handlers only ever set a flag, and the reboot happens
// here once the socket has drained.
void webServerLoop();

// True while a firmware upload is in progress, so the BLE scheduler can stand
// down and leave the radio and CPU to it.
bool webOtaInProgress();
