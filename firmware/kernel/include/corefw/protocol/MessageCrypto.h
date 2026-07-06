// Message-layer crypto: AES-128 payload encryption with an HMAC-SHA256 tag.
//
// This reproduces MeshCore's Utils::encryptThenMAC / MACThenDecrypt byte for
// byte, so direct messages, group messages and requests corefw sends decrypt on
// existing nodes (and vice-versa):
//
//   encryptThenMAC(secret, dest, src, n):
//     ciphertext = AES128-ECB(secret[0..15], pad16(src))   // zero-padded blocks
//     mac        = HMAC-SHA256(secret[0..31], ciphertext)[0..CIPHER_MAC_SIZE-1]
//     dest       = mac || ciphertext                        // MAC prefixes data
//
// The HMAC key is the full 32-byte shared secret; the AES key is its first 16
// bytes (CIPHER_KEY_SIZE). The tag is truncated to CIPHER_MAC_SIZE (2) bytes.
#pragma once

#include <corefw/protocol/Wire.h>

#include <cstdint>
#include <cstring>

extern "C" {
#include "aes128.h"
#include "sha256.h"
}

namespace corefw::proto {

inline constexpr size_t CIPHER_KEY_SIZE = 16;
inline constexpr size_t CIPHER_BLOCK_SIZE = 16;

// HMAC-SHA256 over `data`, writing the first `out_len` bytes of the tag (<= 32)
// to `out`. Key is `key_len` bytes; matches the RFC 2104 construction the
// reference SHA256::resetHMAC/finalizeHMAC uses.
inline void hmacSha256(const uint8_t* key, size_t key_len, const uint8_t* data,
                       size_t data_len, uint8_t* out, size_t out_len) {
  uint8_t k[64];
  std::memset(k, 0, sizeof(k));
  if (key_len > 64) {
    corefw_sha256(key, key_len, k);  // keys longer than the block get hashed
  } else {
    std::memcpy(k, key, key_len);
  }

  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) {
    ipad[i] = uint8_t(k[i] ^ 0x36);
    opad[i] = uint8_t(k[i] ^ 0x5c);
  }

  uint8_t inner[32];
  corefw_sha256_ctx c;
  corefw_sha256_init(&c);
  corefw_sha256_update(&c, ipad, 64);
  corefw_sha256_update(&c, data, data_len);
  corefw_sha256_final(&c, inner);

  uint8_t outer[32];
  corefw_sha256_init(&c);
  corefw_sha256_update(&c, opad, 64);
  corefw_sha256_update(&c, inner, 32);
  corefw_sha256_final(&c, outer);

  if (out_len > 32) out_len = 32;
  std::memcpy(out, outer, out_len);
}

// AES-128-ECB over whole blocks. `src_len` need not be a multiple of 16; the
// final partial block is zero-padded (matching Utils::encrypt). Returns the
// ciphertext length (always a multiple of 16).
inline int aesEncrypt(const uint8_t* secret, uint8_t* dest, const uint8_t* src, int src_len) {
  int out = 0;
  while (src_len >= 16) {
    corefw_aes128_encrypt_block(secret, src, dest + out);
    out += 16;
    src += 16;
    src_len -= 16;
  }
  if (src_len > 0) {
    uint8_t tmp[16];
    std::memset(tmp, 0, 16);
    std::memcpy(tmp, src, src_len);
    corefw_aes128_encrypt_block(secret, tmp, dest + out);
    out += 16;
  }
  return out;
}

inline int aesDecrypt(const uint8_t* secret, uint8_t* dest, const uint8_t* src, int src_len) {
  int out = 0;
  while (out < src_len) {
    corefw_aes128_decrypt_block(secret, src + out, dest + out);
    out += 16;
  }
  return out;  // multiple of 16
}

// encryptThenMAC: writes MAC(CIPHER_MAC_SIZE) || ciphertext to dest, returns
// total length.
inline int encryptThenMAC(const uint8_t* secret, uint8_t* dest, const uint8_t* src, int src_len) {
  int enc_len = aesEncrypt(secret, dest + CIPHER_MAC_SIZE, src, src_len);
  hmacSha256(secret, PUB_KEY_SIZE, dest + CIPHER_MAC_SIZE, enc_len, dest, CIPHER_MAC_SIZE);
  return int(CIPHER_MAC_SIZE) + enc_len;
}

// MACThenDecrypt: verifies the prefix MAC over the ciphertext and, if valid,
// decrypts into dest. Returns plaintext length (multiple of 16) or 0 on a bad
// tag / short input.
inline int MACThenDecrypt(const uint8_t* secret, uint8_t* dest, const uint8_t* src, int src_len) {
  if (src_len <= int(CIPHER_MAC_SIZE)) return 0;
  uint8_t mac[CIPHER_MAC_SIZE];
  hmacSha256(secret, PUB_KEY_SIZE, src + CIPHER_MAC_SIZE, src_len - CIPHER_MAC_SIZE, mac, CIPHER_MAC_SIZE);
  if (std::memcmp(mac, src, CIPHER_MAC_SIZE) != 0) return 0;
  return aesDecrypt(secret, dest, src + CIPHER_MAC_SIZE, src_len - CIPHER_MAC_SIZE);
}

}  // namespace corefw::proto
