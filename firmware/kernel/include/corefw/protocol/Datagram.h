// Datagram builders — construct encrypted Core Protocol packets.
//
// These reproduce the reference firmware's Mesh::createDatagram /
// createGroupDatagram / createAnonDatagram and the BaseChatMesh message
// composition (composeMsgPacket, sendGroupMessage, sendLogin, sendRequest) so
// the packets corefw emits are byte-identical and decrypt on existing nodes.
//
// Every builder leaves packet.header with the payload type set and the route
// bits zero; the caller (dispatcher) sets ROUTE_FLOOD/ROUTE_DIRECT and the path.
#pragma once

#include <corefw/protocol/Identity.h>
#include <corefw/protocol/MessageCrypto.h>
#include <corefw/protocol/Packet.h>
#include <corefw/protocol/Wire.h>

extern "C" {
#include "sha256.h"
}

#include <cstdio>
#include <cstring>

namespace corefw::proto {

// Text message sub-types (TXT_TYPE_* in the reference firmware).
inline constexpr uint8_t TXT_TYPE_PLAIN = 0;
inline constexpr uint8_t TXT_TYPE_CLI_DATA = 1;
inline constexpr uint8_t TXT_TYPE_SIGNED_PLAIN = 2;

// Group datagram data-type sentinels.
inline constexpr uint16_t DATA_TYPE_RESERVED = 0x0000;
inline constexpr uint16_t DATA_TYPE_DEV = 0xFFFF;

// MAX_TEXT_LEN mirrors BaseChatMesh.h (10 * CIPHER_BLOCK_SIZE).
inline constexpr size_t MAX_TEXT_LEN = 10 * CIPHER_BLOCK_SIZE;

// GroupChannel — a symmetric-key channel. The secret is a full 32 bytes (like
// the reference GroupChannel), because the HMAC in encryptThenMAC keys on all
// PUB_KEY_SIZE bytes; a 128-bit channel uses the low 16 with the upper 16 zero.
// `hash` is sha256(secret[:16])[0], matching the reference (which hashes keylen
// bytes = 16 for a 128-bit key).
struct GroupChannel {
  uint8_t hash[PATH_HASH_SIZE] = {};
  uint8_t secret[PUB_KEY_SIZE] = {};

  void setSecret(const uint8_t key[16]) {
    std::memset(secret, 0, sizeof(secret));
    std::memcpy(secret, key, 16);
    uint8_t digest[32];
    corefw_sha256(secret, 16, digest);
    std::memcpy(hash, digest, PATH_HASH_SIZE);
  }
};

// buildDatagram — dest_hash || src_hash || encryptThenMAC(secret, data).
// Used for PAYLOAD_TXT_MSG / PAYLOAD_REQ / PAYLOAD_RESPONSE.
inline bool buildDatagram(Packet& pkt, uint8_t type, const Identity& dest,
                          const LocalIdentity& self, const uint8_t* secret,
                          const uint8_t* data, size_t data_len) {
  if (type != PAYLOAD_TXT_MSG && type != PAYLOAD_REQ && type != PAYLOAD_RESPONSE) return false;
  if (data_len + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE - 1 > MAX_PACKET_PAYLOAD) return false;

  pkt.header = uint8_t(type << PH_TYPE_SHIFT);
  size_t len = 0;
  len += dest.copyHashTo(&pkt.payload[len]);
  len += self.copyHashTo(&pkt.payload[len]);
  len += encryptThenMAC(secret, &pkt.payload[len], data, int(data_len));
  pkt.payload_len = uint16_t(len);
  return true;
}

// buildAnonDatagram — dest_hash || sender_pub_key || encryptThenMAC(secret,
// data). Used for PAYLOAD_ANON_REQ (login and anonymous requests).
inline bool buildAnonDatagram(Packet& pkt, const LocalIdentity& sender, const Identity& dest,
                              const uint8_t* secret, const uint8_t* data, size_t data_len) {
  if (data_len + 1 + PUB_KEY_SIZE + CIPHER_BLOCK_SIZE - 1 > MAX_PACKET_PAYLOAD) return false;
  pkt.header = uint8_t(PAYLOAD_ANON_REQ << PH_TYPE_SHIFT);
  size_t len = 0;
  len += dest.copyHashTo(&pkt.payload[len]);
  std::memcpy(&pkt.payload[len], sender.pub_key, PUB_KEY_SIZE);
  len += PUB_KEY_SIZE;
  len += encryptThenMAC(secret, &pkt.payload[len], data, int(data_len));
  pkt.payload_len = uint16_t(len);
  return true;
}

// buildGroupDatagram — channel_hash || encryptThenMAC(channel.secret, data).
// Used for PAYLOAD_GRP_TXT / PAYLOAD_GRP_DATA.
inline bool buildGroupDatagram(Packet& pkt, uint8_t type, const GroupChannel& channel,
                               const uint8_t* data, size_t data_len) {
  if (type != PAYLOAD_GRP_TXT && type != PAYLOAD_GRP_DATA) return false;
  if (data_len + 1 + CIPHER_BLOCK_SIZE - 1 > MAX_PACKET_PAYLOAD) return false;
  pkt.header = uint8_t(type << PH_TYPE_SHIFT);
  size_t len = 0;
  std::memcpy(&pkt.payload[len], channel.hash, PATH_HASH_SIZE);
  len += PATH_HASH_SIZE;
  len += encryptThenMAC(channel.secret, &pkt.payload[len], data, int(data_len));
  pkt.payload_len = uint16_t(len);
  return true;
}

// --- Plaintext composition (BaseChatMesh) --------------------------------

// composeTextPlaintext builds the inner plaintext for a direct text message:
//   timestamp(4) || (attempt & 3) || text || NUL   (see composeMsgPacket).
// For attempt > 3 a trailing NUL + attempt byte are appended. Returns length.
// Also outputs the expected ACK = sha256(plaintext[0..4+tlen], self.pub_key)[0..3].
inline size_t composeTextPlaintext(uint8_t* temp, uint32_t timestamp, uint8_t attempt,
                                   const char* text, const LocalIdentity& self,
                                   uint32_t& expected_ack) {
  size_t tlen = std::strlen(text);
  putU32LE(temp, 0, timestamp);
  temp[4] = uint8_t(attempt & 3);
  std::memcpy(&temp[5], text, tlen + 1);  // include NUL

  // expected ACK: sha256 over (plaintext[0..5+tlen]) keyed with our pub_key.
  uint8_t digest[32];
  corefw_sha256_ctx c;
  corefw_sha256_init(&c);
  corefw_sha256_update(&c, temp, 5 + tlen);
  corefw_sha256_update(&c, self.pub_key, PUB_KEY_SIZE);
  corefw_sha256_final(&c, digest);
  expected_ack = getU32LE(digest, 0);

  size_t len = 5 + tlen;
  if (attempt > 3) {
    temp[len++] = 0;
    temp[len++] = attempt;
  }
  return len;
}

// composeCliDataPlaintext: timestamp(4) || ((attempt&3)|(TXT_TYPE_CLI_DATA<<2))
// || text. No expected ACK. Returns length (5 + strlen(text)).
inline size_t composeCliDataPlaintext(uint8_t* temp, uint32_t timestamp, uint8_t attempt,
                                      const char* text) {
  size_t tlen = std::strlen(text);
  putU32LE(temp, 0, timestamp);
  temp[4] = uint8_t((attempt & 3) | (TXT_TYPE_CLI_DATA << 2));
  std::memcpy(&temp[5], text, tlen);
  return 5 + tlen;
}

// composeGroupTextPlaintext: timestamp(4) || 0(TXT_TYPE_PLAIN) || "<sender>: <text>".
// Returns length.
inline size_t composeGroupTextPlaintext(uint8_t* temp, uint32_t timestamp,
                                        const char* sender_name, const char* text,
                                        size_t text_len) {
  putU32LE(temp, 0, timestamp);
  temp[4] = 0;  // TXT_TYPE_PLAIN
  int prefix = std::snprintf(reinterpret_cast<char*>(&temp[5]), MAX_TEXT_LEN + 32, "%s: ", sender_name);
  size_t prefix_len = prefix > 0 ? size_t(prefix) : 0;
  if (text_len + prefix_len > MAX_TEXT_LEN) text_len = MAX_TEXT_LEN - prefix_len;
  std::memcpy(&temp[5 + prefix_len], text, text_len);
  temp[5 + prefix_len + text_len] = 0;
  return 5 + prefix_len + text_len;
}

}  // namespace corefw::proto
