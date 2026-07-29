#pragma once

#include <cstddef>
#include <cstdint>

// The persisted layout, the value limits and the clamps live here, apart from
// this header so they can be tested off the target. Included rather than
// forward-declared because everything that touches gConfig needs them.
#include "config_schema.h"

// Runtime configuration, edited from the web UI and persisted to NVS.
//
// The web server's handlers run on the AsyncTCP task, not on loop(), so every
// access goes through configLock()/configUnlock(). Persisting is deferred to
// loop() because NVS writes are slow and must not block the TCP task.

// Loads from NVS, falling back to defaults if absent or written by an
// incompatible build. Creates the config mutex, so call once before any other
// function here.
void configBegin();

// Serial-console dump of the active configuration.
void configPrint();

// Marks the config dirty so loop() flushes it to NVS after a short debounce.
void configMarkDirty();

// Persists to NVS if dirty and the debounce has elapsed. Call from loop().
void configFlushIfDue();

// Persists immediately if dirty, ignoring the debounce.
//
// For use on the way to a restart: the debounce is 1 s but a deferred reboot
// fires after 600 ms, so without this an edit made just before Reboot or Forget
// WiFi was discarded -- after the UI had already said "Saved".
void configFlushNow();

// One draw from a +/-range variance band, in the same real units as the range.
// Shared so the BLE scheduler and the iSpindel poster jitter their readings
// identically rather than growing two subtly different versions.
float randomVariance(float range);

bool configLock();
void configUnlock();

// Only touch this while holding the lock.
extern AppConfig gConfig;
