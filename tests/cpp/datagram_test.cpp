// Host test for the datagram builders: verifies packet structure (dest/src
// hashes, channel hash, anon sender key) and that a receiver holding the same
// shared secret / channel key decrypts the payload back to the exact plaintext.
#include <corefw/protocol/Datagram.h>

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace corefw::proto;

static LocalIdentity idFromByte(uint8_t b) {
  uint8_t seed[SEED_SIZE];
  std::memset(seed, b, sizeof(seed));
  return LocalIdentity::fromSeed(seed);
}

static void testDirectMessage() {
  LocalIdentity alice = idFromByte(1);
  LocalIdentity bob = idFromByte(2);

  uint8_t secret_a[PUB_KEY_SIZE], secret_b[PUB_KEY_SIZE];
  alice.calcSharedSecret(secret_a, bob);
  bob.calcSharedSecret(secret_b, alice);
  assert(std::memcmp(secret_a, secret_b, PUB_KEY_SIZE) == 0);  // ECDH agrees

  uint8_t temp[5 + MAX_TEXT_LEN + 2];
  uint32_t expected_ack = 0;
  size_t plen = composeTextPlaintext(temp, 0x11223344, 0, "hello bob", alice, expected_ack);
  assert(expected_ack != 0);

  Packet pkt;
  assert(buildDatagram(pkt, PAYLOAD_TXT_MSG, bob, alice, secret_a, temp, plen));
  assert(pkt.payloadType() == PAYLOAD_TXT_MSG);

  // Layout: dest_hash(1) || src_hash(1) || MAC(2) || ciphertext.
  assert(pkt.payload[0] == bob.pub_key[0]);   // dest hash
  assert(pkt.payload[1] == alice.pub_key[0]); // src hash

  // Bob decrypts the tail with the shared secret.
  uint8_t dec[MAX_PACKET_PAYLOAD];
  int dlen = MACThenDecrypt(secret_b, dec, &pkt.payload[2], pkt.payload_len - 2);
  assert(dlen > 0);
  assert(getU32LE(dec, 0) == 0x11223344);
  assert(dec[4] == 0);
  assert(std::strcmp(reinterpret_cast<char*>(&dec[5]), "hello bob") == 0);
}

static void testGroupMessage() {
  uint8_t key[16];
  for (int i = 0; i < 16; i++) key[i] = uint8_t(0xA0 + i);
  GroupChannel ch;
  ch.setSecret(key);

  // Channel hash is sha256(secret)[0].
  uint8_t digest[32];
  corefw_sha256(key, 16, digest);
  assert(ch.hash[0] == digest[0]);

  uint8_t temp[5 + MAX_TEXT_LEN + 32];
  size_t plen = composeGroupTextPlaintext(temp, 0x55667788, "alice", "hi all", 6);

  Packet pkt;
  assert(buildGroupDatagram(pkt, PAYLOAD_GRP_TXT, ch, temp, plen));
  assert(pkt.payloadType() == PAYLOAD_GRP_TXT);
  assert(pkt.payload[0] == ch.hash[0]);

  uint8_t dec[MAX_PACKET_PAYLOAD];
  int dlen = MACThenDecrypt(ch.secret, dec, &pkt.payload[1], pkt.payload_len - 1);
  assert(dlen > 0);
  assert(getU32LE(dec, 0) == 0x55667788);
  assert(dec[4] == 0);  // TXT_TYPE_PLAIN
  assert(std::strcmp(reinterpret_cast<char*>(&dec[5]), "alice: hi all") == 0);
}

static void testAnonReq() {
  LocalIdentity client = idFromByte(3);
  LocalIdentity server = idFromByte(4);
  uint8_t secret[PUB_KEY_SIZE];
  client.calcSharedSecret(secret, server);

  uint8_t data[24];
  putU32LE(data, 0, 0xDEADBEEF);        // tag
  std::memcpy(&data[4], "passwd", 6);
  Packet pkt;
  assert(buildAnonDatagram(pkt, client, server, secret, data, 10));
  assert(pkt.payloadType() == PAYLOAD_ANON_REQ);
  // Layout: dest_hash(1) || sender_pub_key(32) || MAC(2) || ciphertext.
  assert(pkt.payload[0] == server.pub_key[0]);
  assert(std::memcmp(&pkt.payload[1], client.pub_key, PUB_KEY_SIZE) == 0);

  uint8_t srv_secret[PUB_KEY_SIZE];
  server.calcSharedSecret(srv_secret, client);
  uint8_t dec[MAX_PACKET_PAYLOAD];
  int dlen = MACThenDecrypt(srv_secret, dec, &pkt.payload[1 + PUB_KEY_SIZE],
                            pkt.payload_len - 1 - PUB_KEY_SIZE);
  assert(dlen > 0);
  assert(getU32LE(dec, 0) == 0xDEADBEEF);
  assert(std::memcmp(&dec[4], "passwd", 6) == 0);
}

int main() {
  testDirectMessage();
  testGroupMessage();
  testAnonReq();
  std::printf("all datagram builder tests passed\n");
  return 0;
}
