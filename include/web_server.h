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

// Whether the server is actually listening. Distinct from webServerBegin()'s
// return: that reports the first attempt, and the bind can succeed on a later
// retry. This is the live answer, and it is what tells a freshly installed
// image apart from one whose admin UI never came back.
bool webServerIsBound();

// Gives up port 80, and stops the rebind retry until webServerResume().
//
// For the setup portal, which runs its own server on the same port. AsyncTCP
// binds with the raw lwIP API and no SO_REUSEADDR, so whichever of the two is
// second simply fails -- and AsyncServer::begin() returns void, so it fails
// silently and the portal would come up unreachable. Standing aside is the only
// way both can exist on one device.
void webServerSuspend();
void webServerResume();
