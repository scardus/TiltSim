#pragma once

#include <esp_heap_caps.h>

#include <cstddef>

// The largest block a byte-wise allocation can actually have.
//
// Deliberately not ESP.getMaxAllocHeap(), which asks for MALLOC_CAP_INTERNAL
// alone (Esp.cpp:150) and so counts the IRAM region that only serves 32-bit
// word accesses. new, malloc and String all need MALLOC_CAP_8BIT and can never
// touch it, so the figure that call reports is partly unreachable -- it sat at
// a suspiciously steady 12276 bytes on a board whose free heap was swinging
// between 32 KB and 13 KB, because it was measuring a region nothing here
// allocates from. This is the number that governs whether an allocation
// succeeds.
//
// Free heap on its own does not answer the question either: fragmentation is
// what stops a large allocation, and the total cannot show it.
inline size_t largestUsableBlock() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
}
