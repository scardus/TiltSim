#include "tilt_encoding.h"

#include <cmath>

namespace {
bool isHexChar(const char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
         (c >= 'a' && c <= 'f');
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c - 'A' + 10;
}

// Canonical UUID string (hyphens optional) to 16 bytes in canonical order,
// most significant first, so uuid[3] is the Tilt colour byte.
bool parseUuid(const char* canonicalUuid, uint8_t* out) {
  if (canonicalUuid == nullptr) {
    return false;
  }
  size_t nibbles = 0;
  for (size_t i = 0; canonicalUuid[i] != '\0'; ++i) {
    const char c = canonicalUuid[i];
    if (c == '-') {
      continue;
    }
    if (!isHexChar(c) || nibbles >= 32) {
      return false;
    }
    if (nibbles % 2 == 0) {
      out[nibbles / 2] = static_cast<uint8_t>(hexValue(c) << 4);
    } else {
      out[nibbles / 2] |= static_cast<uint8_t>(hexValue(c));
    }
    ++nibbles;
  }
  return nibbles == 32;
}

uint16_t clampToUint16(const float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  const long rounded = std::lround(value);
  if (rounded < 0) {
    return 0;
  }
  if (rounded > 65535) {
    return 65535;
  }
  return static_cast<uint16_t>(rounded);
}
}  // namespace

bool buildIBeaconPayload(const char* canonicalUuid, const uint16_t major,
                         const uint16_t minor, const int8_t txPower,
                         IBeaconPayload& out) {
  uint8_t uuid[16];
  if (!parseUuid(canonicalUuid, uuid)) {
    return false;
  }

  size_t i = 0;
  // Company identifiers are little-endian in AD data, so Apple's 0x004C airs
  // as 4C 00. A receiver filtering on the Apple company ID drops the frame if
  // these two are the wrong way round.
  out[i++] = 0x4C;
  out[i++] = 0x00;
  out[i++] = 0x02;  // iBeacon subtype
  out[i++] = 0x15;  // remaining length, always 21
  for (size_t b = 0; b < 16; ++b) {
    out[i++] = uuid[b];
  }
  // Tilt stores both of these big endian. The iBeacon spec does not mandate an
  // order, so this is a Tilt-specific choice and must not be "fixed" to match
  // the little-endian company ID above.
  out[i++] = static_cast<uint8_t>(major >> 8);
  out[i++] = static_cast<uint8_t>(major & 0xFF);
  out[i++] = static_cast<uint8_t>(minor >> 8);
  out[i++] = static_cast<uint8_t>(minor & 0xFF);
  out[i++] = static_cast<uint8_t>(txPower);
  return i == kIBeaconPayloadLen;
}

uint16_t encodeTemperature(const float tempF, const float offsetF, const bool pro) {
  const float adjusted = tempF + offsetF;
  return clampToUint16(pro ? adjusted * 10.0f : adjusted);
}

uint16_t encodeGravity(const float gravity, const float offset, const bool pro) {
  const float adjusted = gravity + offset;
  return clampToUint16(adjusted * (pro ? 10000.0f : 1000.0f));
}

unsigned long rotationDurationMs(const size_t activeCount) {
  return kSliceMs * static_cast<unsigned long>(activeCount);
}

bool tiltBleAddress(const uint8_t* baseMac, const size_t colourIndex,
                    BleAddress& out) {
  if (baseMac == nullptr || colourIndex >= kTiltCount) {
    return false;
  }

  // Top two bits set marks this a static random address; without them the
  // controller rejects the address outright.
  out[0] = static_cast<uint8_t>(0xC0 | (baseMac[0] & 0x3F));
  out[1] = baseMac[1];
  out[2] = baseMac[2];
  out[3] = baseMac[3];
  out[4] = baseMac[4];
  // kTiltCount is 8, so the low three bits hold the colour index outright and
  // no two colours can collide. static_assert below keeps that true.
  out[5] = static_cast<uint8_t>((baseMac[5] & 0xF8) | colourIndex);
  return true;
}

static_assert(kTiltCount <= 8,
              "tiltBleAddress packs the colour index into the low 3 bits of the "
              "address; more than 8 colours would make two of them collide");
