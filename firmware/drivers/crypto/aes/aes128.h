// Compact AES-128 block cipher (ECB single-block encrypt/decrypt).
//
// MeshCore encrypts message payloads with AES-128 used a block at a time (see
// Utils::encrypt/decrypt in the reference firmware, which call
// AES128::encryptBlock/decryptBlock from the rweather Crypto library). corefw
// only needs the raw 16-byte block transform; the ECB chaining and padding live
// in MessageCrypto. This is a standard FIPS-197 implementation, verified on the
// host against the FIPS-197 Appendix B known-answer vector.
#ifndef COREFW_AES128_H
#define COREFW_AES128_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 16-byte key, 16-byte block. Expands the key on each call; MeshCore does the
// same per packet, so there is no state to keep between blocks.
void corefw_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void corefw_aes128_decrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

#ifdef __cplusplus
}
#endif

#endif  // COREFW_AES128_H
