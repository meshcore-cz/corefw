// Core Protocol packet hash.
//
// Identifies a packet for duplicate suppression and ACK matching:
// SHA256(payload_type || [path_len for TRACE] || payload), truncated to
// MAX_HASH_SIZE (8) bytes. This matches Packet::calculatePacketHash in the
// reference firmware, so dedup and ACKs stay compatible.
#pragma once

#include <corefw/protocol/Packet.h>

extern "C" {
#include "sha256.h"
}

namespace corefw::proto {

inline void calculatePacketHash(const Packet& pkt, uint8_t out[MAX_HASH_SIZE]) {
  corefw_sha256_ctx c;
  corefw_sha256_init(&c);
  uint8_t t = pkt.payloadType();
  corefw_sha256_update(&c, &t, 1);
  if (t == PAYLOAD_TRACE) {
    // TRACE packets can revisit the same node on the return path, so the
    // reference firmware folds path_len into the hash to disambiguate.
    uint8_t pl[2] = {uint8_t(pkt.path_len & 0xFF), uint8_t((pkt.path_len >> 8) & 0xFF)};
    corefw_sha256_update(&c, pl, 2);
  }
  corefw_sha256_update(&c, pkt.payload, pkt.payload_len);
  uint8_t full[32];
  corefw_sha256_final(&c, full);
  std::memcpy(out, full, MAX_HASH_SIZE);
}

}  // namespace corefw::proto
