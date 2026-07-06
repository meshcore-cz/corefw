// Host test for the message-crypto layer: AES-128 against the FIPS-197 vector,
// then encryptThenMAC/MACThenDecrypt round-trip and tamper rejection. This is
// the byte-compatibility proof for direct/group message encryption.
#include <corefw/protocol/MessageCrypto.h>

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace corefw::proto;

static void testFips197() {
  // FIPS-197 Appendix B / C.1 known-answer vector.
  const uint8_t key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
                           0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
  const uint8_t pt[16] = {0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,
                          0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34};
  const uint8_t expect[16] = {0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,
                              0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32};
  uint8_t ct[16];
  corefw_aes128_encrypt_block(key, pt, ct);
  assert(std::memcmp(ct, expect, 16) == 0);
  uint8_t back[16];
  corefw_aes128_decrypt_block(key, ct, back);
  assert(std::memcmp(back, pt, 16) == 0);
}

static void testHmacRfc4231() {
  // RFC 4231 test case 1: key = 0x0b*20, data = "Hi There".
  uint8_t key[20];
  std::memset(key, 0x0b, sizeof(key));
  const char* data = "Hi There";
  uint8_t mac[32];
  hmacSha256(key, sizeof(key), reinterpret_cast<const uint8_t*>(data), 8, mac, 32);
  const uint8_t expect[32] = {0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
                              0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
                              0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
                              0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7};
  assert(std::memcmp(mac, expect, 32) == 0);
}

static void testEncryptThenMAC() {
  uint8_t secret[PUB_KEY_SIZE];
  for (size_t i = 0; i < PUB_KEY_SIZE; i++) secret[i] = uint8_t(i * 7 + 1);

  const char* msg = "hello mesh world";  // 16 bytes
  uint8_t enc[128];
  int enc_len = encryptThenMAC(secret, enc, reinterpret_cast<const uint8_t*>(msg), 16);
  // MAC(2) + one 16-byte block.
  assert(enc_len == int(CIPHER_MAC_SIZE) + 16);

  uint8_t dec[128];
  int dec_len = MACThenDecrypt(secret, dec, enc, enc_len);
  assert(dec_len == 16);
  assert(std::memcmp(dec, msg, 16) == 0);

  // Partial block padding: 5-byte plaintext still yields one padded block.
  int e2 = encryptThenMAC(secret, enc, reinterpret_cast<const uint8_t*>("abcde"), 5);
  assert(e2 == int(CIPHER_MAC_SIZE) + 16);
  int d2 = MACThenDecrypt(secret, dec, enc, e2);
  assert(d2 == 16);
  assert(std::memcmp(dec, "abcde", 5) == 0);

  // Tamper the MAC -> rejected.
  enc[0] ^= 0xFF;
  assert(MACThenDecrypt(secret, dec, enc, e2) == 0);
  enc[0] ^= 0xFF;
  // Tamper the ciphertext -> rejected.
  enc[CIPHER_MAC_SIZE] ^= 0x01;
  assert(MACThenDecrypt(secret, dec, enc, e2) == 0);

  // Wrong key -> rejected.
  uint8_t other[PUB_KEY_SIZE];
  std::memset(other, 0xAA, sizeof(other));
  int e3 = encryptThenMAC(secret, enc, reinterpret_cast<const uint8_t*>(msg), 16);
  assert(MACThenDecrypt(other, dec, enc, e3) == 0);
}

int main() {
  testFips197();
  testHmacRfc4231();
  testEncryptThenMAC();
  std::printf("all message-crypto tests passed\n");
  return 0;
}
