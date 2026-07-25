#include <Arduino.h>
#include <unity.h>

#include <algorithm>
#include <string>

#include "tilt_encoding.h"

namespace {
std::string toUpper(const std::string& value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(), [](const unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return out;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_uuid_reversal_matches_known_value() {
  // Red: a495bb10-c5b1-4b44-b512-1370f02d74de, bytes reversed.
  const std::string actual =
      canonicalToBleBeaconUuidInput("a495bb10-c5b1-4b44-b512-1370f02d74de");
  TEST_ASSERT_EQUAL_STRING("DE742DF0-7013-12B5-444B-B1C510BB95A4", actual.c_str());
}

void test_uuid_reversal_is_its_own_inverse() {
  // Reversing twice must return the canonical UUID, for every Tilt colour.
  for (size_t i = 0; i < kTiltCount; ++i) {
    const std::string once = canonicalToBleBeaconUuidInput(kTiltColours[i].uuid);
    const std::string twice = canonicalToBleBeaconUuidInput(once.c_str());
    TEST_ASSERT_EQUAL_STRING(toUpper(kTiltColours[i].uuid).c_str(), twice.c_str());
  }
}

void test_uuid_reversal_shape_for_all_colours() {
  for (size_t i = 0; i < kTiltCount; ++i) {
    const std::string out = canonicalToBleBeaconUuidInput(kTiltColours[i].uuid);
    TEST_ASSERT_EQUAL_UINT32(36, out.length());
    TEST_ASSERT_EQUAL_INT8('-', out[8]);
    TEST_ASSERT_EQUAL_INT8('-', out[13]);
    TEST_ASSERT_EQUAL_INT8('-', out[18]);
    TEST_ASSERT_EQUAL_INT8('-', out[23]);
  }
}

void test_uuid_reversal_rejects_malformed_input() {
  TEST_ASSERT_EQUAL_STRING("", canonicalToBleBeaconUuidInput("").c_str());
  TEST_ASSERT_EQUAL_STRING("", canonicalToBleBeaconUuidInput(nullptr).c_str());
  TEST_ASSERT_EQUAL_STRING("", canonicalToBleBeaconUuidInput("a495bb10").c_str());
  // Valid length, but 'z' is not hex.
  TEST_ASSERT_EQUAL_STRING(
      "", canonicalToBleBeaconUuidInput("z495bb10-c5b1-4b44-b512-1370f02d74de").c_str());
  // One byte too long.
  TEST_ASSERT_EQUAL_STRING(
      "", canonicalToBleBeaconUuidInput("a495bb10-c5b1-4b44-b512-1370f02d74deaa").c_str());
}

void test_standard_encoding() {
  TEST_ASSERT_EQUAL_UINT16(68, encodeTemperature(68.0f, 0.0f, false));
  TEST_ASSERT_EQUAL_UINT16(1053, encodeGravity(1.053f, 0.0f, false));
  // Standard Tilts report whole degrees, so fractions round.
  TEST_ASSERT_EQUAL_UINT16(69, encodeTemperature(68.5f, 0.0f, false));
}

void test_pro_encoding_is_ten_times_standard() {
  TEST_ASSERT_EQUAL_UINT16(685, encodeTemperature(68.5f, 0.0f, true));
  TEST_ASSERT_EQUAL_UINT16(10530, encodeGravity(1.0530f, 0.0f, true));
  TEST_ASSERT_EQUAL_UINT16(680, encodeTemperature(68.0f, 0.0f, true));
}

void test_variance_is_applied_before_scaling() {
  // The bug this guards: a +/-2 degF variance must move a Pro's major by
  // +/-20, not +/-2.
  TEST_ASSERT_EQUAL_UINT16(705, encodeTemperature(68.5f, 2.0f, true));
  TEST_ASSERT_EQUAL_UINT16(665, encodeTemperature(68.5f, -2.0f, true));
  // The same variance on a standard Tilt moves the major by 2.
  TEST_ASSERT_EQUAL_UINT16(70, encodeTemperature(68.0f, 2.0f, false));
  TEST_ASSERT_EQUAL_UINT16(66, encodeTemperature(68.0f, -2.0f, false));
  // Gravity variance scales the same way.
  TEST_ASSERT_EQUAL_UINT16(10540, encodeGravity(1.0530f, 0.0010f, true));
  TEST_ASSERT_EQUAL_UINT16(1054, encodeGravity(1.0530f, 0.0010f, false));
}

void test_out_of_range_values_clamp_rather_than_wrap() {
  TEST_ASSERT_EQUAL_UINT16(0, encodeTemperature(10.0f, -50.0f, false));
  TEST_ASSERT_EQUAL_UINT16(0, encodeGravity(0.0f, 0.0f, false));
  // A Pro at an absurd temperature would overflow 16 bits.
  TEST_ASSERT_EQUAL_UINT16(65535, encodeTemperature(9000.0f, 0.0f, true));
  TEST_ASSERT_EQUAL_UINT16(65535, encodeGravity(9.9f, 0.0f, true));
}

void test_slice_duration_keeps_every_tilt_on_a_five_second_cycle() {
  TEST_ASSERT_EQUAL_UINT32(0, sliceDurationMs(0));
  // Few enough colours to use the full burst window, leaving the cycle quiet.
  TEST_ASSERT_EQUAL_UINT32(1000, sliceDurationMs(1));
  TEST_ASSERT_EQUAL_UINT32(1000, sliceDurationMs(3));
  TEST_ASSERT_EQUAL_UINT32(1000, sliceDurationMs(5));
  // All eight must still fit inside one 5 s cycle.
  TEST_ASSERT_EQUAL_UINT32(625, sliceDurationMs(8));
  TEST_ASSERT_TRUE(sliceDurationMs(kTiltCount) * kTiltCount <= kCyclePeriodMs);
}

void setup() {
  // Give the host serial monitor time to attach before the results stream out.
  delay(2000);
  UNITY_BEGIN();
  RUN_TEST(test_uuid_reversal_matches_known_value);
  RUN_TEST(test_uuid_reversal_is_its_own_inverse);
  RUN_TEST(test_uuid_reversal_shape_for_all_colours);
  RUN_TEST(test_uuid_reversal_rejects_malformed_input);
  RUN_TEST(test_standard_encoding);
  RUN_TEST(test_pro_encoding_is_ten_times_standard);
  RUN_TEST(test_variance_is_applied_before_scaling);
  RUN_TEST(test_out_of_range_values_clamp_rather_than_wrap);
  RUN_TEST(test_slice_duration_keeps_every_tilt_on_a_five_second_cycle);
  UNITY_END();
}

void loop() {}
