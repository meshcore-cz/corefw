// Host test for the mesh receive path: node A builds a datagram (as the command
// handler does), node B's MessageReceiver decrypts it and surfaces the exact
// plaintext. This proves send<->receive interop end to end, independent of the
// radio. Also covers group messages and advert auto-add.
#include <corefw/companion/Receiver.h>
#include <corefw/companion/State.h>
#include <corefw/protocol/Advert.h>
#include <corefw/protocol/Datagram.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace corefw::companion;
namespace proto = corefw::proto;

static int g_fail = 0;
static void check(bool c, const char* w) { if (!c) { std::printf("FAIL: %s\n", w); g_fail = 1; } }

struct RecordingSink : MessageReceiver::Sink {
  std::string last_text, last_channel, last_from_name;
  uint8_t last_txt_type = 255;
  uint32_t last_ts = 0;
  int contact_msgs = 0, channel_msgs = 0, adverts = 0;
  bool last_advert_new = false;

  void onContactMessage(const ContactInfo& from, uint8_t txt_type, uint32_t ts, uint8_t,
                        int8_t, const char* text) override {
    contact_msgs++; last_txt_type = txt_type; last_ts = ts;
    last_text = text; last_from_name = from.name;
  }
  void onChannelMessage(uint8_t, uint32_t ts, uint8_t, int8_t, const char* name,
                        const char* text) override {
    channel_msgs++; last_ts = ts; last_channel = name; last_text = text;
  }
  void onAdvert(const ContactInfo&, bool is_new, uint8_t, const uint8_t*, uint32_t) override {
    adverts++; last_advert_new = is_new;
  }

  int acks = 0; uint32_t last_ack = 0;
  int path_updates = 0;
  int controls = 0; std::string last_control;
  bool need_ack = false; uint32_t need_ack_crc = 0; bool need_ack_flood = false;

  int responses = 0; uint32_t last_resp_tag = 0;
  void onResponse(const ContactInfo&, const uint8_t* d, size_t len) override {
    responses++; if (len >= 4) last_resp_tag = proto::getU32LE(d, 0);
  }
  void onAck(uint32_t crc) override { acks++; last_ack = crc; }
  void onPathUpdated(const ContactInfo&) override { path_updates++; }
  void onControl(const uint8_t* d, size_t len, uint8_t, int8_t) override {
    controls++; last_control.assign(reinterpret_cast<const char*>(d), len);
  }
  void needAck(const ContactInfo&, const uint8_t* ack, uint8_t, bool via_flood) override {
    need_ack = true; need_ack_flood = via_flood; need_ack_crc = proto::getU32LE(ack, 0);
  }
};

static proto::LocalIdentity idOf(uint8_t b) {
  uint8_t seed[proto::SEED_SIZE]; std::memset(seed, b, sizeof(seed));
  return proto::LocalIdentity::fromSeed(seed);
}

static void testDirectMessageInterop() {
  proto::LocalIdentity alice = idOf(1), bob = idOf(2);

  // Bob's state: knows Alice as a contact.
  CompanionState bs; bs.self = bob;
  ContactInfo a; std::memcpy(a.id.pub_key, alice.pub_key, 32);
  std::strcpy(a.name, "Alice"); a.type = ADV_TYPE_CHAT; bs.addContact(a);

  RecordingSink sink; MessageReceiver rx(bs, sink);

  // Alice composes a message to Bob (exactly as the command handler does).
  uint8_t secret[proto::PUB_KEY_SIZE]; alice.calcSharedSecret(secret, bob);
  uint8_t temp[64]; uint32_t ack;
  size_t plen = proto::composeTextPlaintext(temp, 0xABCDEF01, 0, "hi bob", alice, ack);
  proto::Packet pkt;
  proto::buildDatagram(pkt, proto::PAYLOAD_TXT_MSG, bob, alice, secret, temp, plen);
  pkt.setRouteAndType(proto::ROUTE_FLOOD, proto::PAYLOAD_TXT_MSG);

  check(rx.handle(pkt), "bob consumes direct msg");
  check(sink.contact_msgs == 1, "one contact message");
  check(sink.last_text == "hi bob", "decrypted text matches");
  check(sink.last_ts == 0xABCDEF01, "timestamp matches");
  check(sink.last_txt_type == proto::TXT_TYPE_PLAIN, "txt type plain");
  check(sink.last_from_name == "Alice", "from contact matched");

  // A message not addressed to Bob (different dest hash) is ignored.
  proto::LocalIdentity carol = idOf(9);
  if (carol.pub_key[0] != bob.pub_key[0]) {
    proto::Packet other;
    proto::buildDatagram(other, proto::PAYLOAD_TXT_MSG, carol, alice, secret, temp, plen);
    check(!rx.handle(other), "msg for carol ignored by bob");
  }
}

static void testChannelInterop() {
  CompanionState bs; bs.self = idOf(3);
  ChannelDetails ch; std::strcpy(ch.name, "General");
  uint8_t key[16]; for (int i = 0; i < 16; i++) key[i] = uint8_t(i * 3 + 1);
  ch.channel.setSecret(key); bs.setChannel(0, ch);

  RecordingSink sink; MessageReceiver rx(bs, sink);

  uint8_t temp[128];
  size_t plen = proto::composeGroupTextPlaintext(temp, 0x2468, "alice", "hello all", 9);
  proto::Packet pkt;
  proto::buildGroupDatagram(pkt, proto::PAYLOAD_GRP_TXT, ch.channel, temp, plen);
  pkt.setRouteAndType(proto::ROUTE_FLOOD, proto::PAYLOAD_GRP_TXT);

  check(rx.handle(pkt), "consumes channel msg");
  check(sink.channel_msgs == 1, "one channel message");
  check(sink.last_channel == "General", "channel name");
  check(sink.last_text == "alice: hello all", "channel text with sender prefix");
}

static void testAdvertAutoAdd() {
  CompanionState bs; bs.self = idOf(4);
  bs.manual_add_contacts = 0;  // auto-add enabled
  RecordingSink sink; MessageReceiver rx(bs, sink);

  proto::LocalIdentity dave = idOf(7);
  proto::AdvertData ad; ad.type = ADV_TYPE_CHAT; std::strcpy(ad.name, "Dave");
  proto::Packet pkt;
  proto::buildAdvert(pkt, dave, 12345, ad);
  pkt.setRouteAndType(proto::ROUTE_FLOOD, proto::PAYLOAD_ADVERT);

  check(rx.handle(pkt), "consumes advert");
  check(sink.adverts == 1 && sink.last_advert_new, "advert auto-added as new");
  check(bs.num_contacts == 1 && std::strcmp(bs.contacts[0].name, "Dave") == 0, "contact created from advert");

  // Second advert from Dave -> refresh, not new.
  proto::Packet pkt2; proto::buildAdvert(pkt2, dave, 12346, ad);
  pkt2.setRouteAndType(proto::ROUTE_FLOOD, proto::PAYLOAD_ADVERT);
  check(rx.handle(pkt2) && !sink.last_advert_new, "advert refresh not new");
  check(bs.num_contacts == 1, "no duplicate contact");

  // Forged advert (tampered signature) is dropped.
  proto::Packet bad; proto::buildAdvert(bad, dave, 12347, ad);
  bad.payload[proto::PUB_KEY_SIZE + 4] ^= 0xFF;  // corrupt signature
  bad.setRouteAndType(proto::ROUTE_FLOOD, proto::PAYLOAD_ADVERT);
  check(!rx.handle(bad), "forged advert dropped");
}

// Receiving a plain text produces an ACK whose CRC equals the sender's
// expected_ack — the delivery-receipt round trip.
static void testAckGeneration() {
  proto::LocalIdentity alice = idOf(1), bob = idOf(2);
  CompanionState bs; bs.self = bob;
  ContactInfo a; std::memcpy(a.id.pub_key, alice.pub_key, 32);
  std::strcpy(a.name, "Alice"); a.type = ADV_TYPE_CHAT; bs.addContact(a);
  RecordingSink sink; MessageReceiver rx(bs, sink);

  uint8_t secret[proto::PUB_KEY_SIZE]; alice.calcSharedSecret(secret, bob);
  uint8_t temp[64]; uint32_t expected_ack;
  size_t plen = proto::composeTextPlaintext(temp, 0x11223344, 0, "ping", alice, expected_ack);
  proto::Packet pkt;
  proto::buildDatagram(pkt, proto::PAYLOAD_TXT_MSG, bob, alice, secret, temp, plen);
  pkt.setRouteAndType(proto::ROUTE_FLOOD, proto::PAYLOAD_TXT_MSG);

  check(rx.handle(pkt), "bob consumes msg");
  check(sink.need_ack, "bob must acknowledge the plain msg");
  check(sink.need_ack_crc == expected_ack, "generated ACK == sender's expected_ack");
  check(sink.need_ack_flood, "flood message acknowledged via flood");
}

// A received PAYLOAD_ACK surfaces its CRC to the app (delivery confirmation).
static void testAckReception() {
  CompanionState bs; bs.self = idOf(5);
  RecordingSink sink; MessageReceiver rx(bs, sink);
  proto::Packet pkt;
  pkt.setRouteAndType(proto::ROUTE_DIRECT, proto::PAYLOAD_ACK);
  pkt.setPathHashSizeAndCount(1, 0);
  proto::putU32LE(pkt.payload, 0, 0xDEADBEEF);
  pkt.payload_len = 4;
  check(rx.handle(pkt), "consumes ACK");
  check(sink.acks == 1 && sink.last_ack == 0xDEADBEEF, "ACK crc surfaced");
}

// A CONTROL packet (high bit set on byte 0) is surfaced raw.
static void testControlReception() {
  CompanionState bs; bs.self = idOf(6);
  RecordingSink sink; MessageReceiver rx(bs, sink);
  proto::Packet pkt;
  pkt.setRouteAndType(proto::ROUTE_DIRECT, proto::PAYLOAD_CONTROL);
  pkt.setPathHashSizeAndCount(1, 0);
  pkt.payload[0] = 0x80;
  std::memcpy(&pkt.payload[1], "ctl", 3);
  pkt.payload_len = 4;
  check(rx.handle(pkt), "consumes control");
  check(sink.controls == 1 && sink.last_control.size() == 4, "control surfaced");
}

// A PAYLOAD_PATH updates the contact's out_path and surfaces any embedded ACK.
static void testPathReception() {
  proto::LocalIdentity alice = idOf(1), bob = idOf(2);
  CompanionState bs; bs.self = bob;
  ContactInfo a; std::memcpy(a.id.pub_key, alice.pub_key, 32);
  std::strcpy(a.name, "Alice"); a.type = ADV_TYPE_CHAT; a.out_path_len = OUT_PATH_UNKNOWN;
  bs.addContact(a);
  RecordingSink sink; MessageReceiver rx(bs, sink);

  uint8_t secret[proto::PUB_KEY_SIZE]; alice.calcSharedSecret(secret, bob);
  uint8_t temp[64]; size_t k = 0;
  temp[k++] = 2;                        // path_len: 2 one-byte hops
  temp[k++] = 0xAA; temp[k++] = 0xBB;   // the hop hashes
  temp[k++] = proto::PAYLOAD_ACK;       // extra_type
  proto::putU32LE(temp, k, 0xCAFEF00D); k += 4;  // embedded ACK
  // The PATH wire format matches a text datagram (dest‖src‖MAC‖cipher); build the
  // encrypted datagram then relabel the header as PAYLOAD_PATH (corefw only
  // receives path returns, so buildDatagram doesn't emit the type itself).
  proto::Packet pkt;
  proto::buildDatagram(pkt, proto::PAYLOAD_TXT_MSG, bob, alice, secret, temp, k);
  pkt.setRouteAndType(proto::ROUTE_FLOOD, proto::PAYLOAD_PATH);

  check(rx.handle(pkt), "bob consumes path");
  check(sink.path_updates == 1, "path update surfaced");
  ContactInfo* ac = bs.lookupContact(alice.pub_key, 32);
  check(ac && ac->out_path_len == 2, "out_path_len updated");
  check(ac && ac->out_path[0] == 0xAA && ac->out_path[1] == 0xBB, "out_path bytes stored");
  check(sink.acks == 1 && sink.last_ack == 0xCAFEF00D, "embedded ACK surfaced");
}

// A PAYLOAD_RESPONSE (e.g. a repeater login reply) decrypts and surfaces to the
// app so login/status requests can complete.
static void testResponseReception() {
  proto::LocalIdentity repeater = idOf(7), me = idOf(2);
  CompanionState bs; bs.self = me;
  ContactInfo r; std::memcpy(r.id.pub_key, repeater.pub_key, 32);
  std::strcpy(r.name, "Rep"); r.type = ADV_TYPE_CHAT; bs.addContact(r);
  RecordingSink sink; MessageReceiver rx(bs, sink);

  uint8_t secret[proto::PUB_KEY_SIZE]; repeater.calcSharedSecret(secret, me);
  uint8_t temp[16];
  proto::putU32LE(temp, 0, 0x00000099);  // tag / server timestamp
  temp[4] = 0; temp[5] = 1; temp[6] = 1;  // RESP_SERVER_LOGIN_OK, keepalive, admin
  proto::Packet pkt;
  proto::buildDatagram(pkt, proto::PAYLOAD_TXT_MSG, me, repeater, secret, temp, 7);
  pkt.setRouteAndType(proto::ROUTE_DIRECT, proto::PAYLOAD_RESPONSE);
  check(rx.handle(pkt), "consumes response");
  check(sink.responses == 1, "response surfaced");
  check(sink.last_resp_tag == 0x99, "response tag decoded");
}

int main() {
  testDirectMessageInterop();
  testChannelInterop();
  testAdvertAutoAdd();
  testAckGeneration();
  testAckReception();
  testControlReception();
  testPathReception();
  testResponseReception();
  if (g_fail) return 1;
  std::printf("all receiver (RX decrypt) tests passed\n");
  return 0;
}
