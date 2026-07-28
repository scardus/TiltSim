#include <Arduino.h>
#include <unity.h>

#include "tilt_encoding.h"

void setUp() {}
void tearDown() {}

void test_payload_matches_reference_byte_for_byte() {
  // A Red standard Tilt at 68 degF / 1.053 SG, spelled out from the structure
  // in the Tilt iBeacon reference. Every byte of the advertisement is pinned
  // here because this is the project's reference implementation: a receiver
  // built against a wrong payload inherits the fault.
  const uint8_t expected[kIBeaconPayloadLen] = {
    0x4C, 0x00,              // Apple company ID, little-endian in AD data
    0x02, 0x15,              // iBeacon subtype, remaining length
    0xA4, 0x95, 0xBB, 0x10,  // UUID bytes 0-3, byte 3 = Red
    0xC5, 0xB1, 0x4B, 0x44, 0xB5, 0x12, 0x13, 0x70, 0xF0, 0x2D, 0x74, 0xDE,
    0x00, 0x44,              // major 68 degF, big endian
    0x04, 0x1D,              // minor 1053 = 1.053 SG, big endian
    0xF6,                    // measured power -10
  };

  IBeaconPayload actual;
  TEST_ASSERT_TRUE(buildIBeaconPayload("a495bb10-c5b1-4b44-b512-1370f02d74de",
                                       68, 1053, -10, actual));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual.data(), kIBeaconPayloadLen);
}

void test_adv_data_matches_the_canonical_iBeacon_advertisement() {
  // The whole advertisement, flags structure included, byte for byte. This is
  // the guard on building the AD data by hand and handing it to
  // esp_ble_gap_config_adv_data_raw() instead of letting BLEAdvertisementData
  // assemble it: the bytes that leave the antenna must not have changed, or the
  // receiver built against this reference stops recognising the beacon.
  //
  // README documents this exact sequence: 02 01 1A 1A FF 4C 00 02 15 ...
  const uint8_t expected[kAdvDataLen] = {
    0x02, 0x01, 0x1A,        // flags: len 2, type 0x01, LE General + BR/EDR bits
    0x1A, 0xFF,              // manufacturer specific: len 26, type 0xFF
    0x4C, 0x00,              // Apple company ID, little-endian in AD data
    0x02, 0x15,              // iBeacon subtype, remaining length
    0xA4, 0x95, 0xBB, 0x10,  // UUID bytes 0-3, byte 3 = Red
    0xC5, 0xB1, 0x4B, 0x44, 0xB5, 0x12, 0x13, 0x70, 0xF0, 0x2D, 0x74, 0xDE,
    0x00, 0x44,              // major 68 degF, big endian
    0x04, 0x1D,              // minor 1053 = 1.053 SG, big endian
    0xF6,                    // measured power -10
  };

  IBeaconPayload payload;
  TEST_ASSERT_TRUE(buildIBeaconPayload("a495bb10-c5b1-4b44-b512-1370f02d74de",
                                       68, 1053, -10, payload));
  AdvData adv;
  buildAdvData(payload, adv);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, adv.data(), kAdvDataLen);

  // 30 bytes, inside the 31 the legacy advertising PDU allows. One more field
  // would not fit, and the controller would silently truncate rather than fail.
  TEST_ASSERT_EQUAL_UINT32(30, kAdvDataLen);
}

void test_adv_data_length_bytes_describe_their_own_structures() {
  // A length byte that does not match its structure makes a scanner stop
  // parsing at that point, which drops the beacon without any error anywhere.
  IBeaconPayload payload;
  TEST_ASSERT_TRUE(buildIBeaconPayload(kTiltColours[3].uuid, 700, 10500, -59,
                                       payload));
  AdvData adv;
  buildAdvData(payload, adv);

  // Each AD structure is <length> <type> <length-1 bytes of value>, and walking
  // them must land exactly on the end of the buffer.
  size_t offset = 0;
  size_t structures = 0;
  while (offset < kAdvDataLen) {
    const size_t len = adv[offset];
    TEST_ASSERT_TRUE(len > 0);
    offset += 1 + len;
    ++structures;
  }
  TEST_ASSERT_EQUAL_UINT32(kAdvDataLen, offset);
  TEST_ASSERT_EQUAL_UINT32(2, structures);
}

void test_company_id_is_apple_in_air_order() {
  // The regression this guards: the payload used to air 00 4C, advertising
  // company ID 0x4C00, which is not Apple. Receivers that filter on the Apple
  // company ID before parsing the beacon body drop every such frame.
  IBeaconPayload payload;
  TEST_ASSERT_TRUE(buildIBeaconPayload(kTiltColours[0].uuid, 0, 1000, 0, payload));
  TEST_ASSERT_EQUAL_UINT8(0x4C, payload[0]);
  TEST_ASSERT_EQUAL_UINT8(0x00, payload[1]);
  TEST_ASSERT_EQUAL_UINT8(0x02, payload[2]);
  TEST_ASSERT_EQUAL_UINT8(0x15, payload[3]);
}

void test_major_and_minor_are_big_endian() {
  // Deliberately different from the company ID's byte order: Tilt is big endian
  // for these two fields and the iBeacon spec does not say otherwise.
  IBeaconPayload payload;
  TEST_ASSERT_TRUE(
      buildIBeaconPayload(kTiltColours[0].uuid, 0x1234, 0xABCD, 0, payload));
  TEST_ASSERT_EQUAL_UINT8(0x12, payload[20]);
  TEST_ASSERT_EQUAL_UINT8(0x34, payload[21]);
  TEST_ASSERT_EQUAL_UINT8(0xAB, payload[22]);
  TEST_ASSERT_EQUAL_UINT8(0xCD, payload[23]);
}

void test_every_colour_airs_its_uuid_canonically() {
  // Byte 3 carries the colour and must land at payload offset 7, with the
  // trailing 12 bytes identical across every colour.
  const uint8_t kColourBytes[kTiltCount] = {0x10, 0x20, 0x30, 0x40,
                                            0x50, 0x60, 0x70, 0x80};
  for (size_t i = 0; i < kTiltCount; ++i) {
    IBeaconPayload payload;
    TEST_ASSERT_TRUE(
        buildIBeaconPayload(kTiltColours[i].uuid, 70, 1050, -10, payload));
    TEST_ASSERT_EQUAL_UINT8(0xA4, payload[4]);
    TEST_ASSERT_EQUAL_UINT8(0x95, payload[5]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, payload[6]);
    TEST_ASSERT_EQUAL_UINT8(kColourBytes[i], payload[7]);
    TEST_ASSERT_EQUAL_UINT8(0xC5, payload[8]);
    TEST_ASSERT_EQUAL_UINT8(0xDE, payload[19]);
  }
}

void test_payload_rejects_malformed_uuid() {
  IBeaconPayload payload;
  TEST_ASSERT_FALSE(buildIBeaconPayload(nullptr, 68, 1053, -10, payload));
  TEST_ASSERT_FALSE(buildIBeaconPayload("", 68, 1053, -10, payload));
  TEST_ASSERT_FALSE(buildIBeaconPayload("a495bb10", 68, 1053, -10, payload));
  // Valid length, but 'z' is not hex.
  TEST_ASSERT_FALSE(buildIBeaconPayload(
      "z495bb10-c5b1-4b44-b512-1370f02d74de", 68, 1053, -10, payload));
  // One byte too long.
  TEST_ASSERT_FALSE(buildIBeaconPayload(
      "a495bb10-c5b1-4b44-b512-1370f02d74deaa", 68, 1053, -10, payload));
}

void test_payload_accepts_uuid_without_hyphens() {
  // The receiver-side reference lists UUIDs unhyphenated; both must work.
  IBeaconPayload withHyphens;
  IBeaconPayload without;
  TEST_ASSERT_TRUE(buildIBeaconPayload(
      "a495bb30-c5b1-4b44-b512-1370f02d74de", 68, 1053, -10, withHyphens));
  TEST_ASSERT_TRUE(buildIBeaconPayload(
      "A495BB30C5B14B44B5121370F02D74DE", 68, 1053, -10, without));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(withHyphens.data(), without.data(),
                                kIBeaconPayloadLen);
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

void test_rotation_fits_inside_a_receiver_scan_window() {
  TEST_ASSERT_EQUAL_UINT32(0, rotationDurationMs(0));
  TEST_ASSERT_EQUAL_UINT32(200, rotationDurationMs(1));
  TEST_ASSERT_EQUAL_UINT32(800, rotationDurationMs(4));
  TEST_ASSERT_EQUAL_UINT32(1600, rotationDurationMs(8));

  // The point of the whole rotation: even with every colour enabled, a full pass
  // must complete well inside the HM-10's default 3 s scan, or a receiver
  // building a list per scan will keep missing colours.
  const unsigned long kReceiverScanMs = 3000;
  TEST_ASSERT_TRUE(rotationDurationMs(kTiltCount) < kReceiverScanMs);

  // And every colour must air more than once per reading, otherwise a single
  // dropped packet costs a whole 5 s reading.
  TEST_ASSERT_TRUE(rotationDurationMs(kTiltCount) * 2 <= kCyclePeriodMs);
}

void test_addresses_are_valid_static_random() {
  // The Core Spec requires the top two bits set; the controller rejects the
  // address outright otherwise, which would silently stop all advertising.
  const uint8_t base[kBleAddressLen] = {0x24, 0x0A, 0xC4, 0xC2, 0xA0, 0x80};
  for (size_t i = 0; i < kTiltCount; ++i) {
    BleAddress addr;
    TEST_ASSERT_TRUE(tiltBleAddress(base, i, addr));
    TEST_ASSERT_EQUAL_UINT8(0xC0, addr[0] & 0xC0);
  }
}

void test_addresses_are_distinct_for_every_colour() {
  // This is the fix for the receiver collapsing every colour into one discovery
  // list entry, so no two colours may ever share an address.
  const uint8_t base[kBleAddressLen] = {0x24, 0x0A, 0xC4, 0xC2, 0xA0, 0x80};
  BleAddress addrs[kTiltCount];
  for (size_t i = 0; i < kTiltCount; ++i) {
    TEST_ASSERT_TRUE(tiltBleAddress(base, i, addrs[i]));
  }
  for (size_t i = 0; i < kTiltCount; ++i) {
    for (size_t j = i + 1; j < kTiltCount; ++j) {
      TEST_ASSERT_FALSE(addrs[i] == addrs[j]);
    }
  }
}

void test_addresses_are_stable_and_device_specific() {
  const uint8_t base[kBleAddressLen] = {0x24, 0x0A, 0xC4, 0xC2, 0xA0, 0x80};
  const uint8_t other[kBleAddressLen] = {0x24, 0x0A, 0xC4, 0xC2, 0xCC, 0x7C};

  // Stable across calls, so readings stay traceable across a restart.
  BleAddress first;
  BleAddress again;
  TEST_ASSERT_TRUE(tiltBleAddress(base, 3, first));
  TEST_ASSERT_TRUE(tiltBleAddress(base, 3, again));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first.data(), again.data(), kBleAddressLen);

  // The middle bytes carry through, so two boards do not impersonate each other.
  TEST_ASSERT_EQUAL_UINT8(0x0A, first[1]);
  TEST_ASSERT_EQUAL_UINT8(0xC4, first[2]);
  TEST_ASSERT_EQUAL_UINT8(0xC2, first[3]);
  BleAddress otherBoard;
  TEST_ASSERT_TRUE(tiltBleAddress(other, 3, otherBoard));
  TEST_ASSERT_FALSE(first == otherBoard);
}

void test_addresses_reverse_into_the_order_the_stack_expects() {
  // The regression this guards is the Bluedroid-to-NimBLE swap: the two stacks
  // take the six bytes in opposite orders, and the addresses are part of the
  // wire format the receiver keys its discovery list on.
  const uint8_t base[kBleAddressLen] = {0x24, 0x0A, 0xC4, 0xC2, 0xA0, 0x80};
  BleAddress printed;
  TEST_ASSERT_TRUE(tiltBleAddress(base, 5, printed));

  BleAddress little;
  bleAddressLittleEndian(printed, little);

  // ble_hs_id_set_rnd() reads the static-random marker from the last byte and
  // rejects the address without it, which stops advertising entirely.
  TEST_ASSERT_EQUAL_UINT8(0xC0, little[kBleAddressLen - 1] & 0xC0);
  // The colour index travels the other way, from the last byte to the first.
  TEST_ASSERT_EQUAL_UINT8(printed[kBleAddressLen - 1], little[0]);

  // Reversing twice is the identity, so the printed order the serial log shows
  // is still the address that went on air.
  BleAddress roundTrip;
  bleAddressLittleEndian(little, roundTrip);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(printed.data(), roundTrip.data(),
                                kBleAddressLen);

  // In place, which is the aliasing case the copy inside exists for.
  BleAddress inPlace;
  bleAddressLittleEndian(printed, inPlace);
  bleAddressLittleEndian(inPlace, inPlace);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(printed.data(), inPlace.data(), kBleAddressLen);
}

void test_address_rejects_bad_input() {
  BleAddress addr;
  const uint8_t base[kBleAddressLen] = {0x24, 0x0A, 0xC4, 0xC2, 0xA0, 0x80};
  TEST_ASSERT_FALSE(tiltBleAddress(nullptr, 0, addr));
  TEST_ASSERT_FALSE(tiltBleAddress(base, kTiltCount, addr));
  TEST_ASSERT_FALSE(tiltBleAddress(base, 99, addr));
}

void setup() {
  // Give the host serial monitor time to attach before the results stream out.
  delay(2000);
  UNITY_BEGIN();
  RUN_TEST(test_payload_matches_reference_byte_for_byte);
  RUN_TEST(test_adv_data_matches_the_canonical_iBeacon_advertisement);
  RUN_TEST(test_adv_data_length_bytes_describe_their_own_structures);
  RUN_TEST(test_company_id_is_apple_in_air_order);
  RUN_TEST(test_major_and_minor_are_big_endian);
  RUN_TEST(test_every_colour_airs_its_uuid_canonically);
  RUN_TEST(test_payload_rejects_malformed_uuid);
  RUN_TEST(test_payload_accepts_uuid_without_hyphens);
  RUN_TEST(test_standard_encoding);
  RUN_TEST(test_pro_encoding_is_ten_times_standard);
  RUN_TEST(test_variance_is_applied_before_scaling);
  RUN_TEST(test_out_of_range_values_clamp_rather_than_wrap);
  RUN_TEST(test_rotation_fits_inside_a_receiver_scan_window);
  RUN_TEST(test_addresses_are_valid_static_random);
  RUN_TEST(test_addresses_are_distinct_for_every_colour);
  RUN_TEST(test_addresses_are_stable_and_device_specific);
  RUN_TEST(test_addresses_reverse_into_the_order_the_stack_expects);
  RUN_TEST(test_address_rejects_bad_input);
  UNITY_END();
}

void loop() {}
