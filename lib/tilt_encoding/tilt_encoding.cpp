#include "tilt_encoding.h"

#include <cctype>
#include <cmath>

namespace {
bool isHexChar(const char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
         (c >= 'a' && c <= 'f');
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

std::string canonicalToBleBeaconUuidInput(const char* canonicalUuid) {
  if (canonicalUuid == nullptr) {
    return "";
  }

  std::string hexOnly;
  hexOnly.reserve(32);

  for (size_t i = 0; canonicalUuid[i] != '\0'; ++i) {
    const char c = canonicalUuid[i];
    if (c == '-') {
      continue;
    }
    if (!isHexChar(c)) {
      return "";
    }
    hexOnly += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }

  if (hexOnly.length() != 32) {
    return "";
  }

  // BLEBeacon helper expects UUID bytes in reverse order for iBeacon payloads.
  std::string reversedBytes;
  reversedBytes.reserve(32);
  for (int i = 30; i >= 0; i -= 2) {
    reversedBytes += hexOnly.substr(i, 2);
  }

  std::string formatted;
  formatted.reserve(36);
  formatted += reversedBytes.substr(0, 8);
  formatted += "-";
  formatted += reversedBytes.substr(8, 4);
  formatted += "-";
  formatted += reversedBytes.substr(12, 4);
  formatted += "-";
  formatted += reversedBytes.substr(16, 4);
  formatted += "-";
  formatted += reversedBytes.substr(20, 12);
  return formatted;
}

uint16_t encodeTemperature(const float tempF, const float offsetF, const bool pro) {
  const float adjusted = tempF + offsetF;
  return clampToUint16(pro ? adjusted * 10.0f : adjusted);
}

uint16_t encodeGravity(const float gravity, const float offset, const bool pro) {
  const float adjusted = gravity + offset;
  return clampToUint16(adjusted * (pro ? 10000.0f : 1000.0f));
}

unsigned long sliceDurationMs(const size_t activeCount) {
  if (activeCount == 0) {
    return 0;
  }
  const unsigned long fairShare = kCyclePeriodMs / activeCount;
  return fairShare < kMaxSliceMs ? fairShare : kMaxSliceMs;
}
