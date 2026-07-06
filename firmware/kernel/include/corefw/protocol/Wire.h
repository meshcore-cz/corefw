// corefw wire-format primitives.
//
// These little-endian encode/decode helpers exist so the corefw kernel can be
// byte-for-byte compatible with the MeshCore Core Protocol as implemented by
// the reference firmware, while remaining portable host code (no Arduino
// dependency) so the layout can be unit-tested on a workstation.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace corefw::proto {

// Core Protocol V1 sizing constants (see MeshCore.h in the reference firmware).
inline constexpr size_t MAX_HASH_SIZE = 8;
inline constexpr size_t PUB_KEY_SIZE = 32;
inline constexpr size_t PRV_KEY_SIZE = 64;
inline constexpr size_t SEED_SIZE = 32;
inline constexpr size_t SIGNATURE_SIZE = 64;
inline constexpr size_t MAX_ADVERT_DATA_SIZE = 32;
inline constexpr size_t CIPHER_MAC_SIZE = 2;   // V1
inline constexpr size_t PATH_HASH_SIZE = 1;    // V1
inline constexpr size_t MAX_PACKET_PAYLOAD = 184;
inline constexpr size_t MAX_PATH_SIZE = 64;
inline constexpr size_t MAX_TRANS_UNIT = 255;

// putU16LE / putU32LE write a little-endian integer and return the new offset.
inline size_t putU16LE(uint8_t* dst, size_t i, uint16_t v) {
  dst[i++] = uint8_t(v & 0xFF);
  dst[i++] = uint8_t((v >> 8) & 0xFF);
  return i;
}

inline size_t putU32LE(uint8_t* dst, size_t i, uint32_t v) {
  dst[i++] = uint8_t(v & 0xFF);
  dst[i++] = uint8_t((v >> 8) & 0xFF);
  dst[i++] = uint8_t((v >> 16) & 0xFF);
  dst[i++] = uint8_t((v >> 24) & 0xFF);
  return i;
}

inline uint16_t getU16LE(const uint8_t* src, size_t i) {
  return uint16_t(src[i]) | (uint16_t(src[i + 1]) << 8);
}

inline uint32_t getU32LE(const uint8_t* src, size_t i) {
  return uint32_t(src[i]) | (uint32_t(src[i + 1]) << 8) |
         (uint32_t(src[i + 2]) << 16) | (uint32_t(src[i + 3]) << 24);
}

}  // namespace corefw::proto
