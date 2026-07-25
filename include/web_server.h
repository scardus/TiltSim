#pragma once

// Starts the HTTP server on port 80. Call after the network is up.
void webServerBegin();

// True while a firmware upload is in progress, so the BLE scheduler can stand
// down and leave the radio and CPU to it.
bool webOtaInProgress();
