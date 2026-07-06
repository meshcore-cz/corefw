// Host-side identity / advert crypto tests.
//
// Proves corefw's Ed25519 identity is compatible with MeshCore: it signs/verifies
// with the same vendored orlp/ed25519 implementation, reproduces the reference
// firmware's own X25519 key-exchange test vector, and round-trips a signed
// advert (including rejecting a forged signature). Build & run:
//
//   c++ -std=c++17 -I firmware/kernel/include -I firmware/drivers/crypto/ed25519 \
//       firmware/drivers/crypto/ed25519/*.c \
//       firmware/kernel/protocol/identity_test.cpp -o /tmp/idtest && /tmp/idtest
#include <corefw/protocol/Advert.h>
#include <corefw/protocol/Identity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace corefw::proto;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

static void testSignVerify() {
  uint8_t seed[SEED_SIZE];
  for (size_t i = 0; i < SEED_SIZE; ++i) seed[i] = uint8_t(i * 7 + 1);
  LocalIdentity id = LocalIdentity::fromSeed(seed);

  const char* m = "the mesh is the network";
  uint8_t sig[SIGNATURE_SIZE];
  id.sign(sig, reinterpret_cast<const uint8_t*>(m), std::strlen(m));
  check(id.verify(sig, reinterpret_cast<const uint8_t*>(m), std::strlen(m)), "self verify");

  sig[0] ^= 0x01;  // tamper
  check(!id.verify(sig, reinterpret_cast<const uint8_t*>(m), std::strlen(m)), "tampered sig rejected");

  // Serialise (prv||pub) and restore.
  uint8_t blob[PRV_KEY_SIZE + PUB_KEY_SIZE];
  size_t n = id.writeTo(blob, sizeof(blob));
  check(n == PRV_KEY_SIZE + PUB_KEY_SIZE, "serialise length");
  LocalIdentity restored;
  restored.readFrom(blob, n);
  check(restored.matches(id), "restored pub matches");
}

// Reproduces the exact X25519 symmetry the reference firmware relies on in
// LocalIdentity::validatePrivateKey — using its embedded test client keypair.
static void testKeyExchangeVector() {
  const uint8_t test_client_prv[64] = {
      0x70, 0x65, 0xe1, 0x8f, 0xd9, 0xfa, 0xbb, 0x70, 0xc1, 0xed, 0x90, 0xdc, 0xa1,
      0x99, 0x07, 0xde, 0x69, 0x8c, 0x88, 0xb7, 0x09, 0xea, 0x14, 0x6e, 0xaf, 0xd9,
      0x3d, 0x9b, 0x83, 0x0c, 0x7b, 0x60, 0xc4, 0x68, 0x11, 0x93, 0xc7, 0x9b, 0xbc,
      0x39, 0x94, 0x5b, 0xa8, 0x06, 0x41, 0x04, 0xbb, 0x61, 0x8f, 0x8f, 0xd7, 0xa8,
      0x4a, 0x0a, 0xf6, 0xf5, 0x70, 0x33, 0xd6, 0xe8, 0xdd, 0xcd, 0x64, 0x71};
  const uint8_t test_client_pub[32] = {
      0x1e, 0xc7, 0x71, 0x75, 0xb0, 0x91, 0x8e, 0xd2, 0x06, 0xf9, 0xae,
      0x04, 0xec, 0x13, 0x6d, 0x6d, 0x5d, 0x43, 0x15, 0xbb, 0x26, 0x30,
      0x54, 0x27, 0xf6, 0x45, 0xb4, 0x92, 0xe9, 0x35, 0x0c, 0x10};

  // Our node.
  uint8_t seed[SEED_SIZE];
  for (size_t i = 0; i < SEED_SIZE; ++i) seed[i] = uint8_t(0xA0 + i);
  LocalIdentity ours = LocalIdentity::fromSeed(seed);

  uint8_t ss1[32], ss2[32];
  ours.calcSharedSecret(ss1, test_client_pub);            // our prv  + their pub
  ed25519_key_exchange(ss2, ours.pub_key, test_client_prv);  // their prv + our pub
  check(std::memcmp(ss1, ss2, 32) == 0, "ECDH shared secret symmetric (meshcore vector)");
}

static void testAdvertRoundTrip() {
  uint8_t seed[SEED_SIZE];
  for (size_t i = 0; i < SEED_SIZE; ++i) seed[i] = uint8_t(i + 3);
  LocalIdentity id = LocalIdentity::fromSeed(seed);

  AdvertData d;
  d.type = ADV_TYPE_REPEATER;
  d.setLatLon(50.08, 14.42);
  std::strcpy(d.name, "CoreFW Repeater CZ");

  Packet pkt;
  check(buildAdvert(pkt, id, /*timestamp=*/1751800000u, d), "build advert");
  check(pkt.payloadType() == PAYLOAD_ADVERT, "advert payload type");

  // Serialise to wire and back, as it would traverse the radio.
  uint8_t wire[MAX_TRANS_UNIT];
  size_t n = pkt.writeTo(wire);
  Packet rx;
  check(rx.readFrom(wire, n), "advert wire round-trip");

  Identity who;
  uint32_t ts;
  AdvertData got;
  check(parseAdvert(rx, who, ts, got), "parse+verify advert");
  check(who.matches(id), "advert identity matches signer");
  check(ts == 1751800000u, "advert timestamp");
  check(got.type == ADV_TYPE_REPEATER, "advert type");
  check(std::strcmp(got.name, "CoreFW Repeater CZ") == 0, "advert name");
  check(got.lat == 50080000 && got.lon == 14420000, "advert latlon");

  // Forge the signature: verification must fail.
  rx.payload[ADVERT_OFS_SIGNATURE] ^= 0x40;
  Identity who2;
  AdvertData got2;
  uint32_t ts2;
  check(!parseAdvert(rx, who2, ts2, got2), "forged advert rejected");
}

int main() {
  testSignVerify();
  testKeyExchangeVector();
  testAdvertRoundTrip();
  std::printf("all identity/advert crypto tests passed\n");
  return 0;
}
