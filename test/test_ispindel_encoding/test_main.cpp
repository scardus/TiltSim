// Runs both on the target and on a desktop compiler. Everything under test is
// Arduino-free -- ArduinoJson builds on a host too -- so the only thing that has
// to differ is the entry point at the bottom of this file, and Arduino.h itself.
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <ArduinoJson.h>
#include <unity.h>

#include <cstring>

#include "ispindel_encoding.h"

void setUp() {}
void tearDown() {}

namespace {
// Roomy enough for any body this module produces, so a failure means the
// payload is wrong rather than the buffer being mean.
constexpr size_t kBodyLen = 320;

const uint8_t kBase[6] = {0xB0, 0xCB, 0xD8, 0xC2, 0xCC, 0x7C};
}  // namespace

void test_payload_has_every_field_a_receiver_expects() {
  // Pinned against the reference body a real Gravitymon posts. A receiver
  // written against a missing or misnamed key silently records nothing, so
  // every field is asserted rather than spot-checked.
  char body[kBodyLen];
  const IspindelReading reading = {"gravmon", "2E6753", 68.0f, 1.005f};
  TEST_ASSERT_TRUE(buildIspindelJson(reading, body, sizeof(body)) > 0);

  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, body).code());

  TEST_ASSERT_EQUAL_STRING("gravmon", doc["name"]);
  TEST_ASSERT_EQUAL_STRING("2E6753", doc["ID"]);
  TEST_ASSERT_EQUAL_STRING("gravmon", doc["token"]);
  TEST_ASSERT_EQUAL_UINT32(900, doc["interval"].as<uint32_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 68.0f, doc["temperature"].as<float>());
  TEST_ASSERT_EQUAL_STRING("F", doc["temp_units"]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.005f, doc["gravity"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.34f, doc["angle"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.67f, doc["battery"].as<float>());
  TEST_ASSERT_EQUAL_INT(-12, doc["RSSI"].as<int>());
}

void test_standard_payload_has_no_gravitymon_fields() {
  // reading.extended defaults to false when omitted from the aggregate init,
  // same as every call site above -- this is the plain iSpindel format, and it
  // must stay exactly what it was before Gravitymon support existed.
  char body[kBodyLen];
  const IspindelReading reading = {"gravmon", "2E6753", 68.0f, 1.005f};
  TEST_ASSERT_TRUE(buildIspindelJson(reading, body, sizeof(body)) > 0);

  JsonDocument doc;
  deserializeJson(doc, body);
  TEST_ASSERT_FALSE(doc["corr-gravity"].is<float>());
  TEST_ASSERT_FALSE(doc["gravity-unit"].is<const char*>());
  TEST_ASSERT_FALSE(doc["run-time"].is<int>());
}

void test_extended_payload_adds_gravitymon_fields_in_sg() {
  char body[kBodyLen];
  IspindelReading reading = {"gravmon", "2E6753", 68.0f, 1.005f};
  reading.extended = true;
  reading.plato = false;
  TEST_ASSERT_TRUE(buildIspindelJson(reading, body, sizeof(body)) > 0);

  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, body).code());
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.005f, doc["gravity"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.005f, doc["corr-gravity"].as<float>());
  TEST_ASSERT_EQUAL_STRING("G", doc["gravity-unit"]);
  TEST_ASSERT_EQUAL_INT(6, doc["run-time"].as<int>());
}

void test_extended_plato_converts_gravity_and_corr_gravity() {
  // 1.050 SG is the standard reference point for this cubic: real Gravitymon
  // (and every other brewing SG/Plato table) puts it at ~12.4 degP.
  char body[kBodyLen];
  IspindelReading reading = {"gravmon", "2E6753", 68.0f, 1.050f};
  reading.extended = true;
  reading.plato = true;
  TEST_ASSERT_TRUE(buildIspindelJson(reading, body, sizeof(body)) > 0);

  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, body).code());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 12.39f, doc["gravity"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 12.39f, doc["corr-gravity"].as<float>());
  TEST_ASSERT_EQUAL_STRING("P", doc["gravity-unit"]);
}

void test_plato_is_ignored_outside_extended_mode() {
  // A stale plato=true left over from a previous Extended session must not
  // leak into the plain iSpindel format if Extended is switched back off.
  char body[kBodyLen];
  IspindelReading reading = {"gravmon", "2E6753", 68.0f, 1.005f};
  reading.extended = false;
  reading.plato = true;
  TEST_ASSERT_TRUE(buildIspindelJson(reading, body, sizeof(body)) > 0);

  JsonDocument doc;
  deserializeJson(doc, body);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.005f, doc["gravity"].as<float>());
  TEST_ASSERT_FALSE(doc["gravity-unit"].is<const char*>());
}

void test_sg_to_plato_matches_the_reference_conversion() {
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, sgToPlato(1.000f));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 12.39f, sgToPlato(1.050f));
}

void test_temperature_is_always_fahrenheit() {
  // The device stores and posts degF and converts only for display. If this
  // ever flipped, every logged reading would be wrong by the C/F offset while
  // still looking plausible.
  char body[kBodyLen];
  const IspindelReading cold = {"a", "000000", -40.0f, 1.000f};
  const IspindelReading hot = {"a", "000000", 212.0f, 1.000f};

  TEST_ASSERT_TRUE(buildIspindelJson(cold, body, sizeof(body)) > 0);
  JsonDocument doc;
  deserializeJson(doc, body);
  TEST_ASSERT_EQUAL_STRING("F", doc["temp_units"]);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -40.0f, doc["temperature"].as<float>());

  TEST_ASSERT_TRUE(buildIspindelJson(hot, body, sizeof(body)) > 0);
  deserializeJson(doc, body);
  TEST_ASSERT_EQUAL_STRING("F", doc["temp_units"]);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 212.0f, doc["temperature"].as<float>());
}

void test_name_with_json_metacharacters_is_escaped() {
  // The name comes straight from a text box in the web UI. Unescaped, a quote
  // would produce a body no receiver can parse, and the failure would look like
  // a network fault rather than a bad name.
  char body[kBodyLen];
  const IspindelReading reading = {"he said \"hi\" \\ bye", "2E6753", 68.0f,
                                   1.050f};
  TEST_ASSERT_TRUE(buildIspindelJson(reading, body, sizeof(body)) > 0);

  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, body).code());
  TEST_ASSERT_EQUAL_STRING("he said \"hi\" \\ bye", doc["name"]);
}

void test_payload_refuses_to_truncate() {
  // Half a JSON document is worse than none: a receiver would either fail to
  // parse it or, worse, parse a prefix that happens to be valid. Too small a
  // buffer must yield nothing at all.
  char body[16];
  const IspindelReading reading = {"gravmon", "2E6753", 68.0f, 1.005f};
  TEST_ASSERT_EQUAL_UINT32(0, buildIspindelJson(reading, body, sizeof(body)));
  TEST_ASSERT_EQUAL_STRING("", body);
}

void test_payload_rejects_bad_input() {
  char body[kBodyLen];
  const IspindelReading noName = {nullptr, "2E6753", 68.0f, 1.005f};
  const IspindelReading noId = {"gravmon", nullptr, 68.0f, 1.005f};
  TEST_ASSERT_EQUAL_UINT32(0, buildIspindelJson(noName, body, sizeof(body)));
  TEST_ASSERT_EQUAL_UINT32(0, buildIspindelJson(noId, body, sizeof(body)));

  const IspindelReading ok = {"gravmon", "2E6753", 68.0f, 1.005f};
  TEST_ASSERT_EQUAL_UINT32(0, buildIspindelJson(ok, nullptr, sizeof(body)));
  TEST_ASSERT_EQUAL_UINT32(0, buildIspindelJson(ok, body, 0));
}

void test_ids_are_six_uppercase_hex_characters() {
  // Receivers key their device list on this string, so its shape is part of the
  // contract rather than a formatting preference.
  for (size_t i = 0; i < kIspindelCount; ++i) {
    char id[kIspindelIdLen];
    TEST_ASSERT_TRUE(ispindelId(kBase, i, id, sizeof(id)));
    TEST_ASSERT_EQUAL_UINT32(6, strlen(id));
    for (size_t c = 0; c < 6; ++c) {
      const bool isHex = (id[c] >= '0' && id[c] <= '9') ||
                         (id[c] >= 'A' && id[c] <= 'F');
      TEST_ASSERT_TRUE(isHex);
    }
  }
}

void test_ids_are_distinct_for_every_slot() {
  // Four slots sharing an ID would collapse into one device at the receiver,
  // the same failure the per-colour BLE addresses fixed for the Tilts.
  char ids[kIspindelCount][kIspindelIdLen];
  for (size_t i = 0; i < kIspindelCount; ++i) {
    TEST_ASSERT_TRUE(ispindelId(kBase, i, ids[i], kIspindelIdLen));
  }
  for (size_t i = 0; i < kIspindelCount; ++i) {
    for (size_t j = i + 1; j < kIspindelCount; ++j) {
      TEST_ASSERT_TRUE(strcmp(ids[i], ids[j]) != 0);
    }
  }
}

void test_ids_are_stable_and_device_specific() {
  char first[kIspindelIdLen];
  char again[kIspindelIdLen];
  TEST_ASSERT_TRUE(ispindelId(kBase, 2, first, sizeof(first)));
  TEST_ASSERT_TRUE(ispindelId(kBase, 2, again, sizeof(again)));
  // Stable across calls, so readings stay traceable across a restart.
  TEST_ASSERT_EQUAL_STRING(first, again);

  // And derived from the board, so two simulators do not impersonate each other.
  const uint8_t other[6] = {0xB0, 0xCB, 0xD8, 0xC2, 0xA0, 0x80};
  char otherBoard[kIspindelIdLen];
  TEST_ASSERT_TRUE(ispindelId(other, 2, otherBoard, sizeof(otherBoard)));
  TEST_ASSERT_TRUE(strcmp(first, otherBoard) != 0);
}

void test_id_rejects_bad_input() {
  char id[kIspindelIdLen];
  char tooSmall[kIspindelIdLen - 1];
  TEST_ASSERT_FALSE(ispindelId(nullptr, 0, id, sizeof(id)));
  TEST_ASSERT_FALSE(ispindelId(kBase, 0, nullptr, sizeof(id)));
  TEST_ASSERT_FALSE(ispindelId(kBase, kIspindelCount, id, sizeof(id)));
  TEST_ASSERT_FALSE(ispindelId(kBase, 99, id, sizeof(id)));
  TEST_ASSERT_FALSE(ispindelId(kBase, 0, tooSmall, sizeof(tooSmall)));
}

static void runAllTests() {
  RUN_TEST(test_payload_has_every_field_a_receiver_expects);
  RUN_TEST(test_standard_payload_has_no_gravitymon_fields);
  RUN_TEST(test_extended_payload_adds_gravitymon_fields_in_sg);
  RUN_TEST(test_extended_plato_converts_gravity_and_corr_gravity);
  RUN_TEST(test_plato_is_ignored_outside_extended_mode);
  RUN_TEST(test_sg_to_plato_matches_the_reference_conversion);
  RUN_TEST(test_temperature_is_always_fahrenheit);
  RUN_TEST(test_name_with_json_metacharacters_is_escaped);
  RUN_TEST(test_payload_refuses_to_truncate);
  RUN_TEST(test_payload_rejects_bad_input);
  RUN_TEST(test_ids_are_six_uppercase_hex_characters);
  RUN_TEST(test_ids_are_distinct_for_every_slot);
  RUN_TEST(test_ids_are_stable_and_device_specific);
  RUN_TEST(test_id_rejects_bad_input);
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
