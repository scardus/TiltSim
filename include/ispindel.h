#pragma once

#include <cstddef>
#include <cstdint>

// The iSpindel side of the simulator: a timer that POSTs a JSON reading to each
// configured endpoint.
//
// This runs on its own FreeRTOS task rather than from loop(). HTTPClient is
// blocking, and a slow endpoint or a DNS timeout would otherwise stall the BLE
// rotation for seconds at a time -- the tilts would stop advertising while a
// dead URL timed out.

// Derives the per-slot device IDs and starts the posting task. Call once, after
// the network is up; posting is pointless without one.
//
// The task is only started if some slot has an endpoint. Its stack comes out of
// the same DRAM the web server allocates from, so a feature nobody has
// configured should not be holding any of it.
void ispindelBegin();

// Call after any change to the iSpindel settings. Starts the task if a URL has
// just been saved and it was not running, and otherwise wakes it so the new
// settings are exercised now rather than up to fifteen minutes from now --
// which is exactly when someone wants to know whether the endpoint they just
// typed works.
void ispindelNoteConfigChanged();

// The device ID for a slot, as it appears in the posted body. Returns an empty
// string for an out-of-range index, never null, so callers can print it
// directly.
const char* ispindelIdFor(size_t index);

// Bytes still unused on the posting task's stack at its deepest, or 0 when the
// task is not running. Reported for the same reason tcpStackFree is: the stack
// is taken from the heap and held for the life of the device, so its size is
// worth being able to re-check rather than trusting one measurement.
uint32_t ispindelStackFree();
