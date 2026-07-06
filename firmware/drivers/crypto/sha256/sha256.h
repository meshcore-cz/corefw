// Minimal, portable SHA-256 (FIPS 180-4).
//
// corefw uses this for the Core Protocol packet hash (SHA256(type || payload),
// truncated to 8 bytes) that identifies packets for duplicate suppression and
// ACK matching. It must produce the same digest as the reference firmware's
// hash so those elements stay compatible. Public-domain style implementation.
#ifndef COREFW_SHA256_H
#define COREFW_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buffer[64];
  size_t buflen;
} corefw_sha256_ctx;

void corefw_sha256_init(corefw_sha256_ctx* c);
void corefw_sha256_update(corefw_sha256_ctx* c, const uint8_t* data, size_t len);
void corefw_sha256_final(corefw_sha256_ctx* c, uint8_t out[32]);

// One-shot convenience.
void corefw_sha256(const uint8_t* data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif  // COREFW_SHA256_H
