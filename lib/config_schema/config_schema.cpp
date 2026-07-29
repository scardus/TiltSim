#include "config_schema.h"

#include <cmath>

namespace {
float clampFloat(const float value, const float low, const float high) {
  // NaN and the infinities compare false against everything, so they would slip
  // through both bounds below and reach the encoders. Sent to the low end
  // rather than rejected: these arrive from a browser, and the field has to end
  // up holding something a receiver can read.
  if (!std::isfinite(value)) {
    return low;
  }
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

// Case-insensitive prefix test, ASCII only.
//
// Hand-rolled because the obvious spellings are not portable: strncasecmp is
// POSIX and absent from MSVC, _strnicmp is the reverse, and this has to build
// on the ESP32, MinGW and a Linux runner. Schemes are ASCII by definition, so
// there is nothing here a locale could change.
bool hasPrefixIgnoringCase(const char* text, const char* prefix) {
  for (size_t i = 0; prefix[i] != '\0'; ++i) {
    char c = text[i];
    if (c == '\0') {
      return false;
    }
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != prefix[i]) {
      return false;
    }
  }
  return true;
}
}  // namespace

void configClampTilt(TiltSettings& tilt) {
  tilt.tempF = clampFloat(tilt.tempF, kMinTempF, kMaxTempF);
  tilt.gravity = clampFloat(tilt.gravity, kMinGravity, kMaxGravity);
  tilt.tempVarianceF = clampFloat(tilt.tempVarianceF, 0.0f, kMaxTempVarianceF);
  tilt.gravityVariance = clampFloat(tilt.gravityVariance, 0.0f, kMaxGravityVariance);
}

void configClampIspindel(IspindelSettings& ispindel) {
  ispindel.tempF = clampFloat(ispindel.tempF, kMinTempF, kMaxTempF);
  ispindel.gravity = clampFloat(ispindel.gravity, kMinGravity, kMaxGravity);
  ispindel.tempVarianceF =
      clampFloat(ispindel.tempVarianceF, 0.0f, kMaxTempVarianceF);
  ispindel.gravityVariance =
      clampFloat(ispindel.gravityVariance, 0.0f, kMaxGravityVariance);

  // A blob read back from NVS is only as trustworthy as whatever wrote it, and
  // both of these are handed to string functions afterwards.
  ispindel.name[sizeof(ispindel.name) - 1] = '\0';
  ispindel.url[sizeof(ispindel.url) - 1] = '\0';
}

const char* urlProblem(const char* url) {
  if (url == nullptr || url[0] == '\0') {
    return nullptr;  // Empty is how a slot is left unconfigured.
  }
  // Case-insensitive per RFC 3986, which makes the scheme the one part of a URL
  // where case carries no meaning. A user who types HTTP:// has not made a
  // mistake, and refusing it is a puzzle with no clue attached.
  if (hasPrefixIgnoringCase(url, "http://") ||
      hasPrefixIgnoringCase(url, "https://")) {
    return nullptr;
  }
  return "URL must start with http:// or https://";
}
