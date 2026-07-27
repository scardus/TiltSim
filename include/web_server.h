#pragma once

// Registers the routes and starts the HTTP server on port 80. Call after the
// network is up.
//
// Returns whether the server actually bound. False is not fatal: the port can
// still be held by the captive portal's closed-but-lingering sockets, and
// webServerLoop() retries until it clears. AsyncWebServer cannot report this
// itself -- AsyncServer::begin() returns void and only logs "bind error: -8" --
// which is how an unreachable UI used to look like a healthy boot.
bool webServerBegin();

// Retries the bind if it has not succeeded yet, and carries out reboots
// requested by a handler. Call from loop().
//
// Handlers run on the AsyncTCP task, which is watchdogged: calling delay() or
// ESP.restart() from one starves the watchdog and panics the device before the
// response can flush. So handlers only ever set a flag, and the reboot happens
// here once the socket has drained.
void webServerLoop();

// True while a firmware upload is in progress, so the BLE scheduler can stand
// down and leave the radio and CPU to it.
bool webOtaInProgress();
