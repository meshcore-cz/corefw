// Cryptographic identity — Ed25519, compatible with MeshCore.
//
// corefw uses the same Ed25519 implementation (orlp/ed25519, vendored under
// firmware/drivers/crypto/ed25519) and the same key layout as the reference
// firmware, so signatures verify across implementations and X25519 shared
// secrets match. A LocalIdentity holds a keypair on this device; an Identity is
// a remote party whose signatures can be verified.
//
// Layout (matches src/Identity.{h,cpp} in the reference firmware):
//   pub_key : 32 bytes  (Ed25519 public key; the node "hash" is its prefix)
//   prv_key : 64 bytes  (orlp private key = clamped scalar[32] || nonce half[32])
#pragma once

#include <corefw/protocol/Wire.h>

extern "C" {
#include "ed_25519.h"
}

namespace corefw::proto {

class Identity {
 public:
  uint8_t pub_key[PUB_KEY_SIZE] = {};

  Identity() = default;
  explicit Identity(const uint8_t* pub) { std::memcpy(pub_key, pub, PUB_KEY_SIZE); }

  // The path hash is simply the prefix of the public key.
  int copyHashTo(uint8_t* dst, size_t len = PATH_HASH_SIZE) const {
    std::memcpy(dst, pub_key, len);
    return int(len);
  }
  bool isHashMatch(const uint8_t* hash, size_t len = PATH_HASH_SIZE) const {
    return std::memcmp(hash, pub_key, len) == 0;
  }
  bool matches(const Identity& other) const {
    return std::memcmp(pub_key, other.pub_key, PUB_KEY_SIZE) == 0;
  }
  bool matches(const uint8_t* other_pub) const {
    return std::memcmp(pub_key, other_pub, PUB_KEY_SIZE) == 0;
  }

  // Ed25519 signature verification.
  bool verify(const uint8_t* sig, const uint8_t* msg, size_t msg_len) const {
    return ed25519_verify(sig, msg, msg_len, pub_key) != 0;
  }
};

class LocalIdentity : public Identity {
 public:
  uint8_t prv_key[PRV_KEY_SIZE] = {};

  LocalIdentity() = default;

  // Derive a keypair from a 32-byte seed (e.g. from the kernel RNG).
  static LocalIdentity fromSeed(const uint8_t seed[SEED_SIZE]) {
    LocalIdentity id;
    ed25519_create_keypair(id.pub_key, id.prv_key, seed);
    return id;
  }

  void sign(uint8_t* sig, const uint8_t* msg, size_t msg_len) const {
    ed25519_sign(sig, msg, msg_len, pub_key, prv_key);
  }

  // ECDH shared secret (Ed25519 keys transposed to X25519), matching the
  // reference firmware's calcSharedSecret.
  void calcSharedSecret(uint8_t* secret, const uint8_t* other_pub) const {
    ed25519_key_exchange(secret, other_pub, prv_key);
  }
  void calcSharedSecret(uint8_t* secret, const Identity& other) const {
    calcSharedSecret(secret, other.pub_key);
  }

  // Serialise as prv_key || pub_key (the reference on-device format). Returns
  // the number of bytes written (PRV_KEY_SIZE, or PRV+PUB if room).
  size_t writeTo(uint8_t* dst, size_t max_len) const {
    if (max_len < PRV_KEY_SIZE) return 0;
    std::memcpy(dst, prv_key, PRV_KEY_SIZE);
    if (max_len < PRV_KEY_SIZE + PUB_KEY_SIZE) return PRV_KEY_SIZE;
    std::memcpy(dst + PRV_KEY_SIZE, pub_key, PUB_KEY_SIZE);
    return PRV_KEY_SIZE + PUB_KEY_SIZE;
  }

  void readFrom(const uint8_t* src, size_t len) {
    if (len == PRV_KEY_SIZE + PUB_KEY_SIZE) {
      std::memcpy(prv_key, src, PRV_KEY_SIZE);
      std::memcpy(pub_key, src + PRV_KEY_SIZE, PUB_KEY_SIZE);
    } else if (len == PRV_KEY_SIZE) {
      std::memcpy(prv_key, src, PRV_KEY_SIZE);
      ed25519_derive_pub(pub_key, prv_key);
    }
  }
};

}  // namespace corefw::proto
