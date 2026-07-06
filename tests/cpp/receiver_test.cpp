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
  void onAdvert(const ContactInfo&, bool is_new) override { adverts++; last_advert_new = is_new; }
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

int main() {
  testDirectMessageInterop();
  testChannelInterop();
  testAdvertAutoAdd();
  if (g_fail) return 1;
  std::printf("all receiver (RX decrypt) tests passed\n");
  return 0;
}
