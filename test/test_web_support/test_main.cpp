// Runs both on the target and on a desktop compiler; see the entry point at the
// bottom of this file.
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <unity.h>

#include <cstring>

#include "web_support.h"

void setUp() {}
void tearDown() {}

namespace {
constexpr size_t kOutLen = 512;

// Drives assetChunk exactly the way ESPAsyncWebServer drives the filler: it
// accumulates what the callback returns and hands that total back as the next
// index, stopping when the callback reports 0. Reproducing that loop here is
// the whole point -- the bug this guards against is a short fill at a part
// boundary being picked up wrongly on the following call, which needs the
// accumulate-and-resume behaviour to show up at all.
size_t reassemble(const Asset& asset, const size_t maxLen, char* out) {
  size_t index = 0;
  for (;;) {
    uint8_t chunk[kOutLen];
    const size_t written = assetChunk(asset, chunk, maxLen, index);
    if (written == 0) {
      break;
    }
    // A filler that returns more than it was offered overruns the library's
    // buffer, which would be a heap corruption rather than a wrong page.
    TEST_ASSERT_TRUE(written <= maxLen);
    TEST_ASSERT_TRUE(index + written < kOutLen);
    memcpy(out + index, chunk, written);
    index += written;
  }
  out[index] = '\0';
  return index;
}
}  // namespace

void test_asset_reassembles_at_every_chunk_size() {
  // The library picks the buffer size, not us: it depends on the TCP window and
  // on what else is in flight, so a part boundary can land anywhere inside a
  // fill. Every one of these sizes puts the boundaries in a different place.
  const Asset asset = makeAsset("Hello, ", "world", "!");
  const char* const expected = "Hello, world!";

  const size_t sizes[] = {1, 2, 3, 5, 7, 8, 13, 64, 1400};
  for (const size_t maxLen : sizes) {
    char out[kOutLen];
    const size_t total = reassemble(asset, maxLen, out);
    TEST_ASSERT_EQUAL_UINT32(strlen(expected), total);
    TEST_ASSERT_EQUAL_STRING(expected, out);
  }
}

void test_asset_reassembles_the_five_part_page_shape() {
  // The same shape the index page has: five parts of very different sizes, the
  // large ones standing in for the inlined CSS and JS.
  const Asset asset = makeAsset("<html><head>", "body{color:red}", "</head><body>",
                                "console.log(1)", "</body></html>");
  const char* const expected =
      "<html><head>body{color:red}</head><body>console.log(1)</body></html>";

  const size_t sizes[] = {1, 4, 12, 15, 16, 100, 1400};
  for (const size_t maxLen : sizes) {
    char out[kOutLen];
    TEST_ASSERT_EQUAL_UINT32(strlen(expected), reassemble(asset, maxLen, out));
    TEST_ASSERT_EQUAL_STRING(expected, out);
  }
}

void test_a_fill_never_crosses_a_part_boundary() {
  // Deliberate: each part is a separate flash string, so one memcpy cannot span
  // two. Offered far more room than the first part holds, the filler must still
  // stop at the end of it and let the caller come back for the rest.
  const Asset asset = makeAsset("abc", "defgh");
  uint8_t buffer[kOutLen];

  TEST_ASSERT_EQUAL_UINT32(3, assetChunk(asset, buffer, 1400, 0));
  TEST_ASSERT_EQUAL_UINT32(5, assetChunk(asset, buffer, 1400, 3));
  TEST_ASSERT_EQUAL_UINT32(0, assetChunk(asset, buffer, 1400, 8));
}

void test_resuming_mid_part_returns_only_the_remainder() {
  // The short-fill case in isolation: two bytes into a five-byte part, only
  // three are left, however much room is offered.
  const Asset asset = makeAsset("abc", "defgh");
  uint8_t buffer[kOutLen];

  const size_t written = assetChunk(asset, buffer, 1400, 5);
  TEST_ASSERT_EQUAL_UINT32(3, written);
  TEST_ASSERT_EQUAL_UINT8('f', buffer[0]);
  TEST_ASSERT_EQUAL_UINT8('g', buffer[1]);
  TEST_ASSERT_EQUAL_UINT8('h', buffer[2]);
}

void test_exhausted_asset_returns_zero() {
  // Returning 0 is how the library learns the body is complete. Anything else
  // at or past the end either truncates the page or never terminates it.
  const Asset asset = makeAsset("abc", "de");
  uint8_t buffer[kOutLen];

  TEST_ASSERT_EQUAL_UINT32(0, assetChunk(asset, buffer, 1400, asset.total));
  TEST_ASSERT_EQUAL_UINT32(0, assetChunk(asset, buffer, 1400, asset.total + 1));
  TEST_ASSERT_EQUAL_UINT32(0, assetChunk(asset, buffer, 1400, 9999));
}

void test_degenerate_assets_are_safe() {
  uint8_t buffer[kOutLen];

  // Never built by webServerBegin(), but the walk must terminate on one.
  const Asset empty = {};
  TEST_ASSERT_EQUAL_UINT32(0, assetChunk(empty, buffer, 1400, 0));

  const Asset single = makeAsset("only");
  char out[kOutLen];
  TEST_ASSERT_EQUAL_UINT32(4, reassemble(single, 1, out));
  TEST_ASSERT_EQUAL_STRING("only", out);

  // An empty part would make a naive walk stall on it forever, since no offset
  // is ever large enough to move past a zero-length part by subtraction alone.
  const Asset withEmpty = makeAsset("ab", "", "cd");
  TEST_ASSERT_EQUAL_UINT32(4, reassemble(withEmpty, 1, out));
  TEST_ASSERT_EQUAL_STRING("abcd", out);

  // A null buffer is a caller error rather than something the library does, but
  // the alternative to checking is a memcpy to address zero.
  TEST_ASSERT_EQUAL_UINT32(0, assetChunk(single, nullptr, 1400, 0));
}

void test_makeAsset_measures_every_part() {
  const Asset asset = makeAsset("a", "bb", "ccc");
  TEST_ASSERT_EQUAL_UINT32(3, asset.count);
  TEST_ASSERT_EQUAL_UINT32(1, asset.lengths[0]);
  TEST_ASSERT_EQUAL_UINT32(2, asset.lengths[1]);
  TEST_ASSERT_EQUAL_UINT32(3, asset.lengths[2]);
  // total is handed to beginResponse() as the Content-Length, so a wrong sum
  // means the browser waits for bytes that never come, or truncates the page.
  TEST_ASSERT_EQUAL_UINT32(6, asset.total);
}

void test_makeAsset_accepts_a_full_five_parts() {
  // The index page uses all five slots. This is the guard that the cap has not
  // been lowered underneath it; a sixth part no longer compiles at all, which
  // is what the static_assert in makeAsset() is for.
  const Asset asset = makeAsset("1", "2", "3", "4", "5");
  TEST_ASSERT_EQUAL_UINT32(5, asset.count);
  TEST_ASSERT_EQUAL_UINT32(5, asset.total);
  TEST_ASSERT_TRUE(kMaxAssetParts >= 5);
}

void test_upload_is_not_stalled_before_the_threshold() {
  TEST_ASSERT_FALSE(otaStalled(1000, 1000, 15000));   // a chunk just landed
  TEST_ASSERT_FALSE(otaStalled(15999, 1000, 15000));  // 14999 ms of silence
  TEST_ASSERT_FALSE(otaStalled(16000, 1000, 15000));  // exactly at the limit
}

void test_upload_is_stalled_past_the_threshold() {
  TEST_ASSERT_TRUE(otaStalled(16001, 1000, 15000));
  TEST_ASSERT_TRUE(otaStalled(999999, 1000, 15000));
}

void test_a_future_dated_stamp_is_not_a_stall() {
  // The regression this exists for. The stamp is written on the AsyncTCP task
  // and read on the loop task, so it can be a few counts *ahead* of the clock
  // sample. Read unsigned, 1000 - 1001 is 4294967295 rather than -1, and every
  // full-size upload tripped that within seconds -- a 1.7 MB image delivers
  // chunks for seventeen seconds and only one has to land in that window.
  TEST_ASSERT_FALSE(otaStalled(1000, 1001, 15000));
  TEST_ASSERT_FALSE(otaStalled(1000, 5000, 15000));
  TEST_ASSERT_FALSE(otaStalled(0, 1, 15000));
}

void test_stall_detection_survives_a_millis_wraparound() {
  // millis() wraps every 49.7 days, and a device that has been up that long is
  // exactly the one nobody is standing next to. The subtraction is correct
  // across the wrap as long as it stays in 32 bits, which is why otaStalled()
  // takes uint32_t rather than a type that is 64 bits wide on a Linux host.
  TEST_ASSERT_FALSE(otaStalled(10, 0xFFFFFFF0u, 15000));      // 26 ms of silence
  TEST_ASSERT_FALSE(otaStalled(0, 0xFFFFFFFFu, 15000));       // 1 ms
  TEST_ASSERT_TRUE(otaStalled(20000, 0xFFFFFFFFu, 15000));    // 20001 ms
}

static void runAllTests() {
  RUN_TEST(test_asset_reassembles_at_every_chunk_size);
  RUN_TEST(test_asset_reassembles_the_five_part_page_shape);
  RUN_TEST(test_a_fill_never_crosses_a_part_boundary);
  RUN_TEST(test_resuming_mid_part_returns_only_the_remainder);
  RUN_TEST(test_exhausted_asset_returns_zero);
  RUN_TEST(test_degenerate_assets_are_safe);
  RUN_TEST(test_makeAsset_measures_every_part);
  RUN_TEST(test_makeAsset_accepts_a_full_five_parts);
  RUN_TEST(test_upload_is_not_stalled_before_the_threshold);
  RUN_TEST(test_upload_is_stalled_past_the_threshold);
  RUN_TEST(test_a_future_dated_stamp_is_not_a_stall);
  RUN_TEST(test_stall_detection_survives_a_millis_wraparound);
}

#ifdef ARDUINO
void setup() {
  // Give the host serial monitor time to attach before the results stream out.
  delay(2000);
  UNITY_BEGIN();
  runAllTests();
  UNITY_END();
}

void loop() {}
#else
int main() {
  UNITY_BEGIN();
  runAllTests();
  return UNITY_END();
}
#endif
