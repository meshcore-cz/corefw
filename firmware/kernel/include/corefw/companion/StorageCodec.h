// Storage codecs — byte-compatible with the reference companion DataStore.
//
// These serialise/deserialise the on-flash records exactly as MeshCore's
// examples/companion_radio/DataStore.cpp writes them, so a device already
// running MeshCore keeps its identity, preferences, contacts and channels after
// being reflashed with corefw (and vice-versa). The record layouts and file
// names below are the compatibility contract:
//
//   /identity/_main.id : prv_key(64) || pub_key(32) [ || node_name(32) ]
//   /new_prefs         : 137-byte fixed record (see prefs field offsets)
//   /contacts3         : 152 bytes per ContactInfo
//   /channels2         : 68 bytes per channel
//
// All scalars are little-endian (ARM Cortex-M and the host are both LE); floats
// and doubles are IEEE-754, matching the reference which memcpy's them raw.
#pragma once

#include <corefw/companion/State.h>
#include <corefw/protocol/Identity.h>
#include <corefw/protocol/Wire.h>

#include <cstring>

namespace corefw::companion {

// File names (must match DataStore.cpp exactly).
inline constexpr const char* IDENTITY_DIR = "/identity";
inline constexpr const char* IDENTITY_NAME = "_main";      // -> /identity/_main.id
inline constexpr const char* PREFS_FILE = "/new_prefs";
inline constexpr const char* CONTACTS_FILE = "/contacts3";
inline constexpr const char* CHANNELS_FILE = "/channels2";
inline constexpr const char* ADV_BLOBS_FILE = "/adv_blobs";

inline constexpr size_t PREFS_RECORD_SIZE = 137;
inline constexpr size_t CONTACT_RECORD_SIZE = 152;
inline constexpr size_t CHANNEL_RECORD_SIZE = 68;
inline constexpr size_t IDENTITY_RECORD_SIZE = proto::PRV_KEY_SIZE + proto::PUB_KEY_SIZE + 32;

// --- Little-endian float / double helpers -------------------------------
inline void putFloat(uint8_t* p, float v) { std::memcpy(p, &v, 4); }
inline float getFloat(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }
inline void putDouble(uint8_t* p, double v) { std::memcpy(p, &v, 8); }
inline double getDouble(const uint8_t* p) { double v; std::memcpy(&v, p, 8); return v; }

// encodePrefs writes the 137-byte /new_prefs record for `s`. node_lat/lon are
// stored as doubles (degrees); corefw keeps them as micro-degrees in state.
inline void encodePrefs(const CompanionState& s, uint8_t out[PREFS_RECORD_SIZE]) {
  std::memset(out, 0, PREFS_RECORD_SIZE);
  putFloat(&out[0], float(s.airtime_factor_ms) / 1000.0f);       // 0  airtime_factor
  std::memcpy(&out[4], s.node_name, 32);                          // 4  node_name
  // out[36..39] pad
  putDouble(&out[40], double(s.lat_e6) / 1000000.0);             // 40 node_lat
  putDouble(&out[48], double(s.lon_e6) / 1000000.0);             // 48 node_lon
  putFloat(&out[56], float(s.freq_khz) / 1000.0f);              // 56 freq (MHz)
  out[60] = s.sf;                                                 // 60
  out[61] = s.cr;                                                 // 61
  out[62] = s.client_repeat;                                     // 62
  out[63] = s.manual_add_contacts;                              // 63
  putFloat(&out[64], float(s.bw_hz) / 1000.0f);                // 64 bw (kHz)
  out[68] = uint8_t(s.tx_power_dbm);                            // 68
  out[69] = s.telemetry_mode_base;                             // 69
  out[70] = s.telemetry_mode_loc;                             // 70
  out[71] = s.telemetry_mode_env;                            // 71
  putFloat(&out[72], float(s.rx_delay_base_ms) / 1000.0f);    // 72 rx_delay_base
  out[76] = s.advert_loc_policy;                             // 76
  out[77] = s.multi_acks;                                    // 77
  out[78] = s.path_hash_mode;                               // 78
  // out[79] pad
  proto::putU32LE(out, 80, s.ble_pin);                      // 80 ble_pin
  out[84] = 0;                                              // 84 buzzer_quiet
  out[85] = 0;                                              // 85 gps_enabled
  proto::putU32LE(out, 86, 0);                             // 86 gps_interval
  out[87] = s.autoadd_config;                              // 87 autoadd_config
  out[88] = s.autoadd_max_hops;                           // 88 autoadd_max_hops
  out[89] = 0;                                             // 89 rx_boosted_gain
  std::memcpy(&out[90], s.default_scope_name, 31);        // 90 default_scope_name(31)
  std::memcpy(&out[121], s.default_scope_key, 16);        // 121 default_scope_key(16)
}

// decodePrefs reads a 137-byte /new_prefs record into `s`.
inline void decodePrefs(const uint8_t in[PREFS_RECORD_SIZE], CompanionState& s) {
  s.airtime_factor_ms = uint32_t(getFloat(&in[0]) * 1000.0f + 0.5f);
  std::memcpy(s.node_name, &in[4], 32);
  s.node_name[31] = 0;
  s.lat_e6 = int32_t(getDouble(&in[40]) * 1000000.0);
  s.lon_e6 = int32_t(getDouble(&in[48]) * 1000000.0);
  s.freq_khz = uint32_t(getFloat(&in[56]) * 1000.0f + 0.5f);
  s.sf = in[60];
  s.cr = in[61];
  s.client_repeat = in[62];
  s.manual_add_contacts = in[63];
  s.bw_hz = uint32_t(getFloat(&in[64]) * 1000.0f + 0.5f);
  s.tx_power_dbm = int8_t(in[68]);
  s.telemetry_mode_base = in[69];
  s.telemetry_mode_loc = in[70];
  s.telemetry_mode_env = in[71];
  s.rx_delay_base_ms = uint32_t(getFloat(&in[72]) * 1000.0f + 0.5f);
  s.advert_loc_policy = in[76];
  s.multi_acks = in[77];
  s.path_hash_mode = in[78];
  s.ble_pin = proto::getU32LE(in, 80);
  s.autoadd_config = in[87];
  s.autoadd_max_hops = in[88];
  std::memcpy(s.default_scope_name, &in[90], 31);
  s.default_scope_name[30] = 0;
  std::memcpy(s.default_scope_key, &in[121], 16);
}

// encodeContact writes the 152-byte /contacts3 record.
inline void encodeContact(const ContactInfo& c, uint8_t out[CONTACT_RECORD_SIZE]) {
  std::memset(out, 0, CONTACT_RECORD_SIZE);
  size_t i = 0;
  std::memcpy(&out[i], c.id.pub_key, 32); i += 32;
  std::memcpy(&out[i], c.name, 32); i += 32;
  out[i++] = c.type;
  out[i++] = c.flags;
  out[i++] = 0;  // unused
  i = proto::putU32LE(out, i, c.sync_since);
  out[i++] = c.out_path_len;
  i = proto::putU32LE(out, i, c.last_advert_timestamp);
  std::memcpy(&out[i], c.out_path, 64); i += 64;
  i = proto::putU32LE(out, i, c.lastmod);
  i = proto::putU32LE(out, i, uint32_t(c.gps_lat));
  i = proto::putU32LE(out, i, uint32_t(c.gps_lon));
}

// decodeContact reads a 152-byte /contacts3 record.
inline void decodeContact(const uint8_t in[CONTACT_RECORD_SIZE], ContactInfo& c) {
  size_t i = 0;
  std::memcpy(c.id.pub_key, &in[i], 32); i += 32;
  std::memcpy(c.name, &in[i], 32); c.name[31] = 0; i += 32;
  c.type = in[i++];
  c.flags = in[i++];
  i++;  // unused
  c.sync_since = proto::getU32LE(in, i); i += 4;
  c.out_path_len = in[i++];
  c.last_advert_timestamp = proto::getU32LE(in, i); i += 4;
  std::memcpy(c.out_path, &in[i], 64); i += 64;
  c.lastmod = proto::getU32LE(in, i); i += 4;
  c.gps_lat = int32_t(proto::getU32LE(in, i)); i += 4;
  c.gps_lon = int32_t(proto::getU32LE(in, i)); i += 4;
}

// encodeChannel writes the 68-byte /channels2 record. The reference stores a
// 32-byte secret (256-bit capable); corefw uses the low 16 bytes (128-bit).
inline void encodeChannel(const ChannelDetails& ch, uint8_t out[CHANNEL_RECORD_SIZE]) {
  std::memset(out, 0, CHANNEL_RECORD_SIZE);
  // out[0..3] unused
  std::memcpy(&out[4], ch.name, 32);
  std::memcpy(&out[36], ch.channel.secret, 16);  // upper 16 bytes stay zero
}

// decodeChannel reads a 68-byte /channels2 record and recomputes the hash.
inline void decodeChannel(const uint8_t in[CHANNEL_RECORD_SIZE], ChannelDetails& ch) {
  std::memcpy(ch.name, &in[4], 32);
  ch.name[31] = 0;
  uint8_t key[16];
  std::memcpy(key, &in[36], 16);
  ch.channel.setSecret(key);
}

// encodeIdentity writes /identity/_main.id : prv || pub || name(32).
inline void encodeIdentity(const proto::LocalIdentity& id, const char* name,
                           uint8_t out[IDENTITY_RECORD_SIZE]) {
  std::memcpy(out, id.prv_key, proto::PRV_KEY_SIZE);
  std::memcpy(out + proto::PRV_KEY_SIZE, id.pub_key, proto::PUB_KEY_SIZE);
  std::memset(out + proto::PRV_KEY_SIZE + proto::PUB_KEY_SIZE, 0, 32);
  if (name) {
    size_t n = std::strlen(name);
    if (n > 31) n = 31;
    std::memcpy(out + proto::PRV_KEY_SIZE + proto::PUB_KEY_SIZE, name, n);
  }
}

// decodeIdentity reads prv||pub (name optional) from a stored record of length
// `len` (96 for keys only, 128 with a trailing name).
inline void decodeIdentity(const uint8_t* in, size_t len, proto::LocalIdentity& id,
                           char* name, size_t name_cap) {
  if (len < proto::PRV_KEY_SIZE + proto::PUB_KEY_SIZE) {
    id.readFrom(in, len);
    return;
  }
  id.readFrom(in, proto::PRV_KEY_SIZE + proto::PUB_KEY_SIZE);
  if (name && name_cap > 0 && len >= proto::PRV_KEY_SIZE + proto::PUB_KEY_SIZE + 1) {
    size_t n = name_cap - 1;
    if (n > 32) n = 32;
    std::memcpy(name, in + proto::PRV_KEY_SIZE + proto::PUB_KEY_SIZE, n);
    name[n < name_cap ? n : name_cap - 1] = 0;
  }
}

}  // namespace corefw::companion
