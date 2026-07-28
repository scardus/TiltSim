#include "ispindel_encoding.h"

#include <ArduinoJson.h>

#include <cstdio>

bool ispindelId(const uint8_t* baseMac, const size_t index, char* out,
                const size_t outLen) {
  if (baseMac == nullptr || out == nullptr || index >= kIspindelCount ||
      outLen < kIspindelIdLen) {
    return false;
  }

  // The last three bytes are what a board prints on its own label, so an ID
  // derived from them is recognisable as belonging to this device. The slot
  // index replaces the low bits of the final byte, which is what keeps the four
  // apart. kIspindelCount is 4, so two bits are enough; the static_assert below
  // holds that true.
  const uint8_t last = static_cast<uint8_t>((baseMac[5] & 0xFC) | index);
  snprintf(out, outLen, "%02X%02X%02X", baseMac[3], baseMac[4], last);
  return true;
}

static_assert(kIspindelCount <= 4,
              "ispindelId packs the slot index into the low 2 bits of the last "
              "MAC byte; more than 4 slots would make two IDs collide");

size_t buildIspindelJson(const IspindelReading& reading, char* out,
                         const size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return 0;
  }
  out[0] = '\0';
  if (reading.name == nullptr || reading.id == nullptr) {
    return 0;
  }

  // ArduinoJson rather than snprintf because the device name is typed by the
  // user and has to be escaped: a name containing a quote would otherwise emit
  // a body the receiver cannot parse.
  JsonDocument doc;
  doc["name"] = reading.name;
  doc["ID"] = reading.id;
  doc["token"] = kPlaceholderToken;
  doc["interval"] = kIspindelIntervalSec;
  doc["temperature"] = reading.tempF;
  doc["temp_units"] = kIspindelTempUnits;
  doc["gravity"] = reading.gravity;
  doc["angle"] = kPlaceholderAngle;
  doc["battery"] = kPlaceholderBattery;
  doc["RSSI"] = kPlaceholderRssi;

  // measureJson excludes the terminator, so a body that exactly fills the
  // buffer still needs one byte more than it reports.
  if (measureJson(doc) + 1 > outLen) {
    return 0;
  }
  return serializeJson(doc, out, outLen);
}
