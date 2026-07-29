#include "web_support.h"

size_t assetChunk(const Asset& asset, uint8_t* const buffer, const size_t maxLen,
                  const size_t index) {
  if (buffer == nullptr) {
    return 0;
  }

  // Walk to the part holding this offset, subtracting each whole part passed.
  size_t offset = index;
  size_t part = 0;
  while (part < asset.count && offset >= asset.lengths[part]) {
    offset -= asset.lengths[part];
    ++part;
  }
  // Past the end: either the asset is exhausted or it was empty to begin with.
  if (part >= asset.count) {
    return 0;
  }

  // Only ever fills to the end of the current part, never across a boundary.
  // The caller comes back for the rest with a larger index.
  const size_t remaining = asset.lengths[part] - offset;
  const size_t chunk = remaining < maxLen ? remaining : maxLen;
  memcpy(buffer, asset.parts[part] + offset, chunk);
  return chunk;
}

bool otaStalled(const uint32_t nowMs, const uint32_t lastChunkMs,
                const uint32_t thresholdMs) {
  const int32_t silentMs = static_cast<int32_t>(nowMs - lastChunkMs);
  return silentMs > static_cast<int32_t>(thresholdMs);
}
