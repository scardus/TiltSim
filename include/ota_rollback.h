#pragma once

// Automatic recovery from a firmware update that installs but does not work.
//
// A freshly OTA'd image boots once on probation. Unless this code declares it
// healthy, the bootloader puts the previous image back at the next restart --
// so a bad update costs a reboot rather than a trip to wherever the device is
// with a USB cable.
//
// Reads the state left by the bootloader and logs it. Call early in setup(),
// after Serial is up.
void otaRollbackBegin();

// Confirms the running image once it has proved itself, or gives up on it and
// falls back. Call from loop().
void otaRollbackLoop();

// The running image's probation state, for /api/state: "pending verify" until
// confirmed, "valid" after, "undefined" for an image flashed over USB (which is
// never on probation). Read live from the OTA data partition rather than
// remembered, so it reports what the bootloader will actually act on.
const char* otaRollbackState();
