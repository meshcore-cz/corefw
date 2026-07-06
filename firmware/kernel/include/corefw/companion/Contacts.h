// Contact, channel and offline-message models for the companion role.
//
// These mirror the reference firmware's ContactInfo / ChannelDetails and the
// companion frame encodings (updateContactFromFrame / writeContactRespFrame in
// examples/companion_radio/MyMesh.cpp). They are portable and host-testable; a
// board plugs in persistence through the ContactStore interface.
#pragma once

#include <corefw/companion/Protocol.h>
#include <corefw/protocol/Datagram.h>
#include <corefw/protocol/Identity.h>
#include <corefw/protocol/Wire.h>

#include <cstdint>
#include <cstring>

namespace corefw::companion {

// OUT_PATH_UNKNOWN marks a contact with no known return path (send by flood).
inline constexpr uint8_t OUT_PATH_UNKNOWN = 0xFF;

// ContactInfo — a known peer. Field order follows the reference so the frame
// codecs below are simple memcpy sequences.
struct ContactInfo {
  proto::Identity id;                             // pub_key (32)
  uint8_t type = 0;                               // ADV_TYPE_*
  uint8_t flags = 0;
  uint8_t out_path_len = OUT_PATH_UNKNOWN;
  uint8_t out_path[proto::MAX_PATH_SIZE] = {};
  char name[32] = {};
  uint32_t last_advert_timestamp = 0;
  int32_t gps_lat = 0;
  int32_t gps_lon = 0;
  uint32_t lastmod = 0;
  uint32_t sync_since = 0;                         // rooms: last synced message time

  bool prefixMatches(const uint8_t* prefix, size_t plen) const {
    return std::memcmp(id.pub_key, prefix, plen) == 0;
  }
};

// updateContactFromFrame decodes a CMD_ADD_UPDATE_CONTACT frame into `c`.
// `last_mod` receives the frame's lastmod (or the caller's fallback if absent).
// Layout: code(1) pub_key(32) type(1) flags(1) out_path_len(1) out_path(64)
//         name(32) last_advert(4) [gps_lat(4) gps_lon(4) [lastmod(4)]].
inline void updateContactFromFrame(ContactInfo& c, uint32_t& last_mod,
                                   const uint8_t* frame, int len) {
  int i = 1;  // skip command code
  std::memcpy(c.id.pub_key, &frame[i], proto::PUB_KEY_SIZE);
  i += proto::PUB_KEY_SIZE;
  c.type = frame[i++];
  c.flags = frame[i++];
  c.out_path_len = frame[i++];
  std::memcpy(c.out_path, &frame[i], proto::MAX_PATH_SIZE);
  i += proto::MAX_PATH_SIZE;
  std::memcpy(c.name, &frame[i], 32);
  c.name[31] = 0;
  i += 32;
  c.last_advert_timestamp = proto::getU32LE(frame, i);
  i += 4;
  if (len >= i + 8) {
    c.gps_lat = int32_t(proto::getU32LE(frame, i)); i += 4;
    c.gps_lon = int32_t(proto::getU32LE(frame, i)); i += 4;
    if (len >= i + 4) last_mod = proto::getU32LE(frame, i);
  }
}

// writeContactRespFrame encodes a RESP_CODE_CONTACT / PUSH_CODE_NEW_ADVERT frame.
// Returns the frame length.
inline size_t writeContactRespFrame(uint8_t code, const ContactInfo& c, uint8_t* out) {
  size_t i = 0;
  out[i++] = code;
  std::memcpy(&out[i], c.id.pub_key, proto::PUB_KEY_SIZE);
  i += proto::PUB_KEY_SIZE;
  out[i++] = c.type;
  out[i++] = c.flags;
  out[i++] = c.out_path_len;
  std::memcpy(&out[i], c.out_path, proto::MAX_PATH_SIZE);
  i += proto::MAX_PATH_SIZE;
  // strzcpy: null-terminated within 32 bytes.
  std::memset(&out[i], 0, 32);
  std::strncpy(reinterpret_cast<char*>(&out[i]), c.name, 31);
  i += 32;
  i = proto::putU32LE(out, i, c.last_advert_timestamp);
  i = proto::putU32LE(out, i, uint32_t(c.gps_lat));
  i = proto::putU32LE(out, i, uint32_t(c.gps_lon));
  i = proto::putU32LE(out, i, c.lastmod);
  return i;
}

// ChannelDetails — a named symmetric channel slot.
struct ChannelDetails {
  char name[32] = {};
  proto::GroupChannel channel;
};

// OfflineMessage — a received message frame queued for CMD_SYNC_NEXT_MESSAGE.
struct OfflineMessage {
  uint8_t buf[MAX_FRAME_SIZE] = {};
  int len = 0;
};

}  // namespace corefw::companion
