// Runs both on the target and on a desktop compiler; see the entry point at the
// bottom of this file.
//
// The layout assertions below are target truth: AppConfig is written to NVS as
// one blob, so the ESP32's idea of its size and offsets is the one that decides
// whether a stored blob still reads back correctly. They are asserted on the
// host too because both compilers agree today, and knowing if that ever stops
// being true is worth having.
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <unity.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "config_schema.h"

void setUp() {}
void tearDown() {}

void test_config_magic_is_TIL2() {
  TEST_ASSERT_EQUAL_HEX32(0x54494C32, kConfigMagic);
}

void test_app_config_layout_is_pinned_to_the_magic() {
  // configBegin() accepts a stored blob when the magic matches and the length
  // matches. That catches a field being added or removed, because sizeof moves.
  // What it cannot catch is a change that keeps sizeof the same -- two floats
  // swapped, a bool relocated -- and the consequence of that is worse than a
  // reset to defaults: the blob is read straight into the new layout, every
  // value silently belongs to a different field, and eight tilts come back
  // holding each other's settings.
  //
  // So the offsets are pinned here. If this test fails, the layout changed:
  // bump kConfigMagic in config_schema.h so stored settings are discarded
  // rather than misread, then update the numbers below.
  TEST_ASSERT_EQUAL_UINT32(20, sizeof(TiltSettings));
  TEST_ASSERT_EQUAL_UINT32(0, offsetof(TiltSettings, enabled));
  TEST_ASSERT_EQUAL_UINT32(1, offsetof(TiltSettings, pro));
  TEST_ASSERT_EQUAL_UINT32(4, offsetof(TiltSettings, tempF));
  TEST_ASSERT_EQUAL_UINT32(8, offsetof(TiltSettings, gravity));
  TEST_ASSERT_EQUAL_UINT32(12, offsetof(TiltSettings, tempVarianceF));
  TEST_ASSERT_EQUAL_UINT32(16, offsetof(TiltSettings, gravityVariance));

  TEST_ASSERT_EQUAL_UINT32(180, sizeof(IspindelSettings));
  TEST_ASSERT_EQUAL_UINT32(0, offsetof(IspindelSettings, enabled));
  TEST_ASSERT_EQUAL_UINT32(1, offsetof(IspindelSettings, name));
  TEST_ASSERT_EQUAL_UINT32(33, offsetof(IspindelSettings, url));
  TEST_ASSERT_EQUAL_UINT32(164, offsetof(IspindelSettings, tempF));
  TEST_ASSERT_EQUAL_UINT32(168, offsetof(IspindelSettings, gravity));
  TEST_ASSERT_EQUAL_UINT32(172, offsetof(IspindelSettings, tempVarianceF));
  TEST_ASSERT_EQUAL_UINT32(176, offsetof(IspindelSettings, gravityVariance));

  TEST_ASSERT_EQUAL_UINT32(888, sizeof(AppConfig));
  TEST_ASSERT_EQUAL_UINT32(0, offsetof(AppConfig, magic));
  TEST_ASSERT_EQUAL_UINT32(4, offsetof(AppConfig, masterEnabled));
  TEST_ASSERT_EQUAL_UINT32(8, offsetof(AppConfig, tilts));
  TEST_ASSERT_EQUAL_UINT32(168, offsetof(AppConfig, ispindels));

  // The array sizes come from the encoding libraries, so a colour or a slot
  // added there silently changes what is persisted here.
  TEST_ASSERT_EQUAL_UINT32(8, kTiltCount);
  TEST_ASSERT_EQUAL_UINT32(4, kIspindelCount);
}

void test_tilt_values_in_range_are_left_alone() {
  TiltSettings tilt = {};
  tilt.tempF = 68.5f;
  tilt.gravity = 1.053f;
  tilt.tempVarianceF = 2.0f;
  tilt.gravityVariance = 0.002f;
  configClampTilt(tilt);

  TEST_ASSERT_EQUAL_FLOAT(68.5f, tilt.tempF);
  TEST_ASSERT_EQUAL_FLOAT(1.053f, tilt.gravity);
  TEST_ASSERT_EQUAL_FLOAT(2.0f, tilt.tempVarianceF);
  TEST_ASSERT_EQUAL_FLOAT(0.002f, tilt.gravityVariance);
}

void test_tilt_values_out_of_range_are_pulled_back() {
  TiltSettings low = {};
  low.tempF = -1000.0f;
  low.gravity = 0.0f;
  low.tempVarianceF = -5.0f;      // a variance is a magnitude, never negative
  low.gravityVariance = -1.0f;
  configClampTilt(low);
  TEST_ASSERT_EQUAL_FLOAT(kMinTempF, low.tempF);
  TEST_ASSERT_EQUAL_FLOAT(kMinGravity, low.gravity);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, low.tempVarianceF);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, low.gravityVariance);

  TiltSettings high = {};
  high.tempF = 9000.0f;
  high.gravity = 99.0f;
  high.tempVarianceF = 1000.0f;
  high.gravityVariance = 50.0f;
  configClampTilt(high);
  TEST_ASSERT_EQUAL_FLOAT(kMaxTempF, high.tempF);
  TEST_ASSERT_EQUAL_FLOAT(kMaxGravity, high.gravity);
  TEST_ASSERT_EQUAL_FLOAT(kMaxTempVarianceF, high.tempVarianceF);
  TEST_ASSERT_EQUAL_FLOAT(kMaxGravityVariance, high.gravityVariance);
}

void test_non_finite_tilt_values_go_to_the_low_end() {
  // NaN and the infinities compare false against every bound, so an unguarded
  // clamp passes them straight through to encodeTemperature(). Sent to the low
  // end rather than rejected, deliberately: these arrive from a browser, and the
  // field has to end up holding something a receiver can read. Infinity landing
  // on the *minimum* looks odd but is the safe direction -- a garbage reading
  // should not present as a plausible hot one.
  TiltSettings tilt = {};
  tilt.tempF = NAN;
  tilt.gravity = INFINITY;
  tilt.tempVarianceF = -INFINITY;
  tilt.gravityVariance = NAN;
  configClampTilt(tilt);

  TEST_ASSERT_EQUAL_FLOAT(kMinTempF, tilt.tempF);
  TEST_ASSERT_EQUAL_FLOAT(kMinGravity, tilt.gravity);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, tilt.tempVarianceF);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, tilt.gravityVariance);
}

void test_ispindel_numbers_clamp_like_a_tilt() {
  IspindelSettings ispindel = {};
  ispindel.tempF = 9000.0f;
  ispindel.gravity = NAN;
  ispindel.tempVarianceF = -3.0f;
  ispindel.gravityVariance = 99.0f;
  configClampIspindel(ispindel);

  TEST_ASSERT_EQUAL_FLOAT(kMaxTempF, ispindel.tempF);
  TEST_ASSERT_EQUAL_FLOAT(kMinGravity, ispindel.gravity);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, ispindel.tempVarianceF);
  TEST_ASSERT_EQUAL_FLOAT(kMaxGravityVariance, ispindel.gravityVariance);
}

void test_ispindel_strings_are_forced_terminated() {
  // This is the one clamp whose failure is a buffer overrun rather than a wrong
  // reading. The blob comes back from NVS and is only as trustworthy as whatever
  // wrote it -- an older build, a corrupted sector, a shorter struct -- and both
  // of these fields are then handed to strlen, strlcpy and Serial.printf.
  IspindelSettings ispindel;
  memset(&ispindel, 'A', sizeof(ispindel));
  ispindel.tempF = 68.0f;
  ispindel.gravity = 1.05f;
  ispindel.tempVarianceF = 0.0f;
  ispindel.gravityVariance = 0.0f;

  configClampIspindel(ispindel);

  TEST_ASSERT_EQUAL_UINT8('\0', ispindel.name[kIspindelNameLen - 1]);
  TEST_ASSERT_EQUAL_UINT8('\0', ispindel.url[kIspindelUrlLen - 1]);
  // And the fields are now safe to measure, which is the point.
  TEST_ASSERT_EQUAL_UINT32(kIspindelNameLen - 1, strlen(ispindel.name));
  TEST_ASSERT_EQUAL_UINT32(kIspindelUrlLen - 1, strlen(ispindel.url));
}

void test_ispindel_strings_within_bounds_survive() {
  IspindelSettings ispindel = {};
  snprintf(ispindel.name, sizeof(ispindel.name), "fermenter 1");
  snprintf(ispindel.url, sizeof(ispindel.url), "http://example.com/log?id=7");
  configClampIspindel(ispindel);

  TEST_ASSERT_EQUAL_STRING("fermenter 1", ispindel.name);
  TEST_ASSERT_EQUAL_STRING("http://example.com/log?id=7", ispindel.url);
}

void test_empty_url_is_how_a_slot_is_left_unconfigured() {
  TEST_ASSERT_NULL(urlProblem(""));
  // Not something a handler passes today, but the alternative to checking is a
  // read through a null pointer.
  TEST_ASSERT_NULL(urlProblem(nullptr));
}

void test_http_and_https_urls_are_accepted() {
  TEST_ASSERT_NULL(urlProblem("http://example.com"));
  TEST_ASSERT_NULL(urlProblem("https://example.com"));
  TEST_ASSERT_NULL(urlProblem("http://192.168.0.92/api/log?id=1&t=2"));
}

void test_url_schemes_are_case_insensitive() {
  // RFC 3986 makes the scheme the one part of a URL where case carries no
  // meaning. This used to be a plain strncmp, so a user who typed HTTP:// was
  // told their URL was invalid with no hint as to why.
  TEST_ASSERT_NULL(urlProblem("HTTP://example.com"));
  TEST_ASSERT_NULL(urlProblem("HTTPS://example.com"));
  TEST_ASSERT_NULL(urlProblem("HtTpS://example.com"));
  TEST_ASSERT_NULL(urlProblem("Http://example.com"));
}

void test_other_schemes_are_rejected() {
  // Rejected rather than clamped: there is no nearest-valid-URL to snap to, and
  // silently dropping the edit would leave the box showing something the device
  // is not using.
  TEST_ASSERT_NOT_NULL(urlProblem("ftp://example.com"));
  TEST_ASSERT_NOT_NULL(urlProblem("javascript:alert(1)"));
  TEST_ASSERT_NOT_NULL(urlProblem("file:///etc/passwd"));
  TEST_ASSERT_NOT_NULL(urlProblem("example.com"));
  TEST_ASSERT_NOT_NULL(urlProblem("//example.com"));
  // One slash short, and a truncated scheme -- the second walks off the end of
  // the string mid-comparison.
  TEST_ASSERT_NOT_NULL(urlProblem("http:/example.com"));
  TEST_ASSERT_NOT_NULL(urlProblem("http"));
  TEST_ASSERT_NOT_NULL(urlProblem("h"));
}

static void runAllTests() {
  RUN_TEST(test_config_magic_is_TIL2);
  RUN_TEST(test_app_config_layout_is_pinned_to_the_magic);
  RUN_TEST(test_tilt_values_in_range_are_left_alone);
  RUN_TEST(test_tilt_values_out_of_range_are_pulled_back);
  RUN_TEST(test_non_finite_tilt_values_go_to_the_low_end);
  RUN_TEST(test_ispindel_numbers_clamp_like_a_tilt);
  RUN_TEST(test_ispindel_strings_are_forced_terminated);
  RUN_TEST(test_ispindel_strings_within_bounds_survive);
  RUN_TEST(test_empty_url_is_how_a_slot_is_left_unconfigured);
  RUN_TEST(test_http_and_https_urls_are_accepted);
  RUN_TEST(test_url_schemes_are_case_insensitive);
  RUN_TEST(test_other_schemes_are_rejected);
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
