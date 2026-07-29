#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Arduino-free logic lifted out of src/web_server.cpp, for the same reason
// tilt_encoding exists: both of the things here are pure, both have failure
// modes that are invisible from outside the device, and neither could be
// reached by a test while it sat in a translation unit that pulls in
// ESPAsyncWebServer.

// ---------------------------------------------------------------------------
// A page assembled from several flash strings, served as one response.
//
// The parts exist so the stylesheet is stored once and still emitted into both
// pages: the alternative, a literal copy in each, costs ~6 KB of flash on a
// build already well into its slot.
// ---------------------------------------------------------------------------

constexpr size_t kMaxAssetParts = 5;

struct Asset {
  const char* parts[kMaxAssetParts];
  size_t lengths[kMaxAssetParts];
  size_t count;
  size_t total;
};

// Variadic rather than taking a std::initializer_list, so the part count is a
// compile-time value and the static_assert below can see it.
//
// The list version silently dropped anything past kMaxAssetParts: it broke out
// of its loop and returned a short Asset, so the page went on air missing
// whichever piece came last -- with no error anywhere, on a build where the
// index page already uses all five slots. Adding a sixth part is a reasonable
// thing to want to do; finding out from a half-rendered page in a browser is
// not. Now it fails the build and says which constant to raise.
template <typename... Ts>
Asset makeAsset(Ts... parts) {
  static_assert(sizeof...(Ts) <= kMaxAssetParts,
                "too many asset parts: raise kMaxAssetParts, or the extra ones "
                "are dropped and the page is served incomplete");
  // The trailing nullptr keeps this a valid array declaration when the pack is
  // empty; the loop below stops at sizeof...(Ts), so it is never read.
  const char* const list[] = {parts..., nullptr};

  Asset asset = {};
  for (size_t i = 0; i < sizeof...(Ts); ++i) {
    asset.parts[i] = list[i];
    asset.lengths[i] = strlen(list[i]);
    asset.total += asset.lengths[i];
  }
  asset.count = sizeof...(Ts);
  return asset;
}

// Copies the slice of an asset starting at `index` into `buffer`, returning how
// many bytes were written, or 0 once the asset is exhausted.
//
// ESPAsyncWebServer accumulates what this returns and passes it back as the
// next `index`, so a short fill at a part boundary has to be picked up
// correctly on the following call. Getting that wrong serves a page with a gap
// or a repeat in it, which no log would ever show.
size_t assetChunk(const Asset& asset, uint8_t* buffer, size_t maxLen,
                  size_t index);

// ---------------------------------------------------------------------------
// OTA upload stall detection.
// ---------------------------------------------------------------------------

// Whether an upload has gone quiet for longer than thresholdMs.
//
// uint32_t rather than unsigned long on purpose. millis() is 32-bit on this
// target, but `unsigned long` is 64-bit on a Linux host and 32-bit under
// MinGW -- so a test of the wraparound case written against that type would
// exercise arithmetic the device never performs, and would do it differently
// depending on which machine ran it. Fixed widths make the target, the local
// host and the CI runner all test the same thing.
//
// The subtraction is deliberately done unsigned and *then* read as signed. The
// two timestamps are sampled on different tasks, so lastChunkMs can legitimately
// be a few counts ahead of nowMs; written as an unsigned comparison that reads
// as 4294967295 rather than -1, which aborted healthy uploads within seconds.
// See the call site in webServerLoop() for the full account.
bool otaStalled(uint32_t nowMs, uint32_t lastChunkMs, uint32_t thresholdMs);
