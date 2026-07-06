// AdvertData — the application-data blob carried inside an ADVERT packet.
//
// Bit-identical to the reference firmware's AdvertDataBuilder / AdvertDataParser
// (helpers/AdvertDataHelpers.cpp), so adverts emitted by a corefw node are
// understood by existing MeshCore nodes and vice-versa.
#pragma once

#include <corefw/protocol/Wire.h>

namespace corefw::proto {

// Advert node types (low nibble of the flags byte).
enum AdvertType : uint8_t {
  ADV_TYPE_NONE = 0,
  ADV_TYPE_CHAT = 1,
  ADV_TYPE_REPEATER = 2,
  ADV_TYPE_ROOM = 3,
  ADV_TYPE_SENSOR = 4,
};

// Flag bits (high nibble).
inline constexpr uint8_t ADV_LATLON_MASK = 0x10;
inline constexpr uint8_t ADV_FEAT1_MASK = 0x20;
inline constexpr uint8_t ADV_FEAT2_MASK = 0x40;
inline constexpr uint8_t ADV_NAME_MASK = 0x80;

// AdvertData encodes/decodes the advert app_data blob.
struct AdvertData {
  uint8_t type = ADV_TYPE_NONE;
  bool has_loc = false;
  int32_t lat = 0;  // degrees * 1e6
  int32_t lon = 0;  // degrees * 1e6
  uint16_t feat1 = 0;
  uint16_t feat2 = 0;
  char name[MAX_ADVERT_DATA_SIZE + 1] = {};

  void setLatLon(double lat_deg, double lon_deg) {
    has_loc = true;
    lat = int32_t(lat_deg * 1e6);
    lon = int32_t(lon_deg * 1e6);
  }

  // encode writes the blob into app_data (>= MAX_ADVERT_DATA_SIZE) and returns
  // the encoded length. Field order matches the reference firmware exactly:
  // flags, [lat, lon], [feat1], [feat2], name.
  uint8_t encode(uint8_t* app_data) const {
    uint8_t flags = uint8_t(type & 0x0F);
    size_t i = 1;
    if (has_loc) {
      flags |= ADV_LATLON_MASK;
      i = putU32LE(app_data, i, uint32_t(lat));
      i = putU32LE(app_data, i, uint32_t(lon));
    }
    if (feat1) {
      flags |= ADV_FEAT1_MASK;
      i = putU16LE(app_data, i, feat1);
    }
    if (feat2) {
      flags |= ADV_FEAT2_MASK;
      i = putU16LE(app_data, i, feat2);
    }
    if (name[0] != 0) {
      flags |= ADV_NAME_MASK;
      for (const char* sp = name; *sp && i < MAX_ADVERT_DATA_SIZE; ++sp) {
        app_data[i++] = uint8_t(*sp);
      }
    }
    app_data[0] = flags;
    return uint8_t(i);
  }

  // decode parses a blob. Returns false on truncation.
  bool decode(const uint8_t* app_data, uint8_t len) {
    if (len < 1) return false;
    uint8_t flags = app_data[0];
    type = uint8_t(flags & 0x0F);
    has_loc = (flags & ADV_LATLON_MASK) != 0;
    feat1 = feat2 = 0;
    name[0] = 0;
    size_t i = 1;
    if (flags & ADV_LATLON_MASK) {
      if (i + 8 > len) return false;
      lat = int32_t(getU32LE(app_data, i)); i += 4;
      lon = int32_t(getU32LE(app_data, i)); i += 4;
    }
    if (flags & ADV_FEAT1_MASK) {
      if (i + 2 > len) return false;
      feat1 = getU16LE(app_data, i); i += 2;
    }
    if (flags & ADV_FEAT2_MASK) {
      if (i + 2 > len) return false;
      feat2 = getU16LE(app_data, i); i += 2;
    }
    if (len < i) return false;
    if (flags & ADV_NAME_MASK) {
      size_t nlen = len - i;
      if (nlen > MAX_ADVERT_DATA_SIZE) nlen = MAX_ADVERT_DATA_SIZE;
      std::memcpy(name, &app_data[i], nlen);
      name[nlen] = 0;
    }
    return true;
  }
};

// buildAdvertSignedMessage assembles the exact byte sequence the reference
// firmware signs/verifies for an ADVERT: pub_key || timestamp(LE) || app_data.
// The caller signs the returned buffer with Ed25519.
inline size_t buildAdvertSignedMessage(uint8_t* msg, const uint8_t* pub_key,
                                       uint32_t timestamp, const uint8_t* app_data,
                                       uint8_t app_data_len) {
  size_t n = 0;
  std::memcpy(&msg[n], pub_key, PUB_KEY_SIZE); n += PUB_KEY_SIZE;
  n = putU32LE(msg, n, timestamp);
  std::memcpy(&msg[n], app_data, app_data_len); n += app_data_len;
  return n;
}

}  // namespace corefw::proto
