// Host-side Companion Protocol command-handler tests.
//
// Feeds inbound command frames and asserts the exact response bytes, matching
// the reference companion firmware. Covers every command group: device/config,
// contacts, channels, messaging (with decrypt round-trip), requests, signing,
// and the offline sync queue.
#include <corefw/companion/Commands.h>
#include <corefw/protocol/MessageCrypto.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace corefw::companion;
namespace proto = corefw::proto;

static int g_fail = 0;
static void check(bool cond, const char* what) {
  if (!cond) { std::printf("FAIL: %s\n", what); g_fail = 1; }
}

// Collects frames the handler emits.
class Collector : public FrameWriter {
 public:
  std::vector<std::vector<uint8_t>> frames;
  void writeFrame(const uint8_t* d, size_t n) override {
    frames.emplace_back(d, d + n);
  }
  const std::vector<uint8_t>& last() const { return frames.back(); }
  void clear() { frames.clear(); }
};

class FakeHost : public CompanionHost {
 public:
  uint32_t now = 1000000;
  bool prefs_saved = false, contacts_saved = false, channels_saved = false;
  bool rebooted = false;
  uint32_t rtcNow() override { return now; }
  void setRtc(uint32_t s) override { now = s; }
  uint16_t batteryMilliVolts() override { return 3700; }
  uint32_t storageUsedKb() override { return 12; }
  uint32_t storageTotalKb() override { return 1024; }
  const char* manufacturerName() override { return "Seeed Studio"; }
  void savePrefs() override { prefs_saved = true; }
  void saveContacts() override { contacts_saved = true; }
  void saveChannels() override { channels_saved = true; }
  void reboot() override { rebooted = true; }
  bool privateKeyExportEnabled() override { return true; }
};

// Records sent packets and lets tests decrypt them.
class FakeSender : public MeshSender {
 public:
  std::vector<proto::Packet> sent;
  bool self_advert = false, self_advert_flood = false;
  int outcome = SEND_FLOOD;
  uint32_t est = 4321;
  uint32_t uniq = 2000000;

  int sendToContact(proto::Packet& pkt, const ContactInfo&, uint32_t& est_timeout) override {
    sent.push_back(pkt); est_timeout = est; return outcome;
  }
  bool sendGroup(proto::Packet& pkt) override { sent.push_back(pkt); return true; }
  bool sendDirect(proto::Packet& pkt, const uint8_t*, uint8_t) override { sent.push_back(pkt); return true; }
  bool sendZeroHop(proto::Packet& pkt) override { sent.push_back(pkt); return true; }
  bool sendRawPacket(const uint8_t*, size_t, uint8_t) override { return true; }
  bool sendSelfAdvert(bool flood) override { self_advert = true; self_advert_flood = flood; return true; }
  uint32_t rtcNowUnique() override { return uniq++; }
  uint32_t random32() override { return 0xA5A5A5A5; }
};

// Build a state with a self identity and one known contact.
struct Fixture {
  CompanionState s;
  FakeHost h;
  FakeSender tx;
  proto::LocalIdentity peer;

  Fixture() {
    uint8_t seed[proto::SEED_SIZE];
    std::memset(seed, 0x11, sizeof(seed));
    s.self = proto::LocalIdentity::fromSeed(seed);
    std::memset(seed, 0x22, sizeof(seed));
    peer = proto::LocalIdentity::fromSeed(seed);

    ContactInfo c;
    std::memcpy(c.id.pub_key, peer.pub_key, proto::PUB_KEY_SIZE);
    std::strcpy(c.name, "peer");
    c.type = ADV_TYPE_CHAT;
    c.out_path_len = OUT_PATH_UNKNOWN;
    c.lastmod = 500;
    s.addContact(c);
  }
  CommandHandler handler() { return CommandHandler(s, h, tx); }
};

static void testDeviceQuery() {
  Fixture f; f.s.ble_pin = 0x00010203;
  Collector out; auto cmd = f.handler();
  uint8_t in[] = {CMD_DEVICE_QUERY, 0x0D};
  cmd.handle(in, sizeof(in), out);
  const auto& r = out.last();
  check(r[0] == RESP_CODE_DEVICE_INFO, "device_info code");
  check(r[1] == FIRMWARE_VER_CODE, "ver code 13");
  check(r[2] == f.s.max_contacts / 2, "max contacts/2");
  check(r[3] == f.s.max_group_channels, "max channels");
  check(proto::getU32LE(r.data(), 4) == f.s.ble_pin, "ble pin LE");
  check(f.s.app_target_ver == 0x0D, "app ver stored");
}

static void testSelfInfo() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t in[8] = {CMD_APP_START, 0,0,0,0,0,0,0};
  cmd.handle(in, sizeof(in), out);
  const auto& r = out.last();
  check(r[0] == RESP_CODE_SELF_INFO, "self_info code");
  check(r[1] == ADV_TYPE_CHAT, "adv type chat");
  check(std::memcmp(&r[4], f.s.self.pub_key, proto::PUB_KEY_SIZE) == 0, "self pubkey");
}

static void testDeviceTime() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t get[] = {CMD_GET_DEVICE_TIME};
  cmd.handle(get, 1, out);
  check(out.last()[0] == RESP_CODE_CURR_TIME, "curr time code");
  check(proto::getU32LE(out.last().data(), 1) == f.h.now, "curr time value");
  out.clear();
  uint8_t set[5]; set[0] = CMD_SET_DEVICE_TIME; proto::putU32LE(set, 1, f.h.now + 100);
  cmd.handle(set, 5, out);
  check(out.last()[0] == RESP_CODE_OK, "set time ok");
  check(f.h.now == 1000100, "time advanced");
  out.clear();
  proto::putU32LE(set, 1, 5);  // past -> reject
  cmd.handle(set, 5, out);
  check(out.last()[0] == RESP_CODE_ERR && out.last()[1] == ERR_ILLEGAL_ARG, "past time rejected");
}

static void testSetNameAndTxPower() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t nm[] = {CMD_SET_ADVERT_NAME, 'N','o','d','e','X'};
  cmd.handle(nm, sizeof(nm), out);
  check(out.last()[0] == RESP_CODE_OK && std::strcmp(f.s.node_name, "NodeX") == 0, "set name");
  check(f.h.prefs_saved, "prefs saved on name");
  out.clear();
  uint8_t tp[] = {CMD_SET_RADIO_TX_POWER, 20};
  cmd.handle(tp, 2, out);
  check(out.last()[0] == RESP_CODE_OK && f.s.tx_power_dbm == 20, "set tx power");
  out.clear();
  uint8_t bad[] = {CMD_SET_RADIO_TX_POWER, uint8_t(int8_t(-30))};
  cmd.handle(bad, 2, out);
  check(out.last()[0] == RESP_CODE_ERR, "tx power out of range rejected");
}

static void testRadioParams() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t in[11];
  in[0] = CMD_SET_RADIO_PARAMS;
  proto::putU32LE(in, 1, 868000);   // 868 MHz in kHz
  proto::putU32LE(in, 5, 250000);   // 250 kHz in Hz
  in[9] = 10; in[10] = 5;
  cmd.handle(in, 11, out);
  check(out.last()[0] == RESP_CODE_OK, "radio params ok");
  check(f.s.freq_khz == 868000 && f.s.bw_hz == 250000 && f.s.sf == 10 && f.s.cr == 5, "radio params stored");
}

static void testBattAndStorage() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t in[] = {CMD_GET_BATT_AND_STORAGE};
  cmd.handle(in, 1, out);
  const auto& r = out.last();
  check(r[0] == RESP_CODE_BATT_AND_STORAGE, "batt code");
  check(proto::getU16LE(r.data(), 1) == 3700, "batt mv");
  check(proto::getU32LE(r.data(), 3) == 12, "used kb");
  check(proto::getU32LE(r.data(), 7) == 1024, "total kb");
}

static void testSelfAdvert() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t in[] = {CMD_SEND_SELF_ADVERT, 1};
  cmd.handle(in, 2, out);
  check(out.last()[0] == RESP_CODE_OK, "self advert ok");
  check(f.tx.self_advert && f.tx.self_advert_flood, "self advert flood requested");
}

static void testContacts() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  // GET_CONTACTS: START, one CONTACT, END.
  uint8_t gc[] = {CMD_GET_CONTACTS};
  cmd.handle(gc, 1, out);
  check(out.frames.size() == 3, "contacts: start+1+end");
  check(out.frames[0][0] == RESP_CODE_CONTACTS_START, "contacts start");
  check(proto::getU32LE(out.frames[0].data(), 1) == 1, "contacts count 1");
  check(out.frames[1][0] == RESP_CODE_CONTACT, "contact frame");
  check(std::memcmp(&out.frames[1][1], f.peer.pub_key, proto::PUB_KEY_SIZE) == 0, "contact pubkey");
  check(out.frames[2][0] == RESP_CODE_END_OF_CONTACTS, "contacts end");

  // ADD_UPDATE a new contact.
  out.clear();
  uint8_t add[1 + 32 + 2 + 1 + 64 + 32 + 4] = {};
  size_t i = 0; add[i++] = CMD_ADD_UPDATE_CONTACT;
  uint8_t newkey[32]; std::memset(newkey, 0x33, 32);
  std::memcpy(&add[i], newkey, 32); i += 32;
  add[i++] = ADV_TYPE_CHAT;  // type
  add[i++] = 0;              // flags
  add[i++] = OUT_PATH_UNKNOWN; // out_path_len
  i += proto::MAX_PATH_SIZE;  // out_path
  std::strcpy(reinterpret_cast<char*>(&add[i]), "newpeer"); i += 32;  // name
  i += 4;  // last_advert
  cmd.handle(add, i, out);
  check(out.last()[0] == RESP_CODE_OK, "add contact ok");
  check(f.s.num_contacts == 2, "contact added");

  // GET_CONTACT_BY_KEY.
  out.clear();
  uint8_t gk[1 + 32]; gk[0] = CMD_GET_CONTACT_BY_KEY; std::memcpy(&gk[1], newkey, 32);
  cmd.handle(gk, sizeof(gk), out);
  check(out.last()[0] == RESP_CODE_CONTACT, "get by key");

  // REMOVE_CONTACT.
  out.clear();
  uint8_t rm[1 + 32]; rm[0] = CMD_REMOVE_CONTACT; std::memcpy(&rm[1], newkey, 32);
  cmd.handle(rm, sizeof(rm), out);
  check(out.last()[0] == RESP_CODE_OK && f.s.num_contacts == 1, "remove contact");

  // RESET_PATH on the original peer.
  out.clear();
  uint8_t rp[1 + 32]; rp[0] = CMD_RESET_PATH; std::memcpy(&rp[1], f.peer.pub_key, 32);
  cmd.handle(rp, sizeof(rp), out);
  check(out.last()[0] == RESP_CODE_OK, "reset path ok");
}

static void testChannels() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t set[2 + 32 + 16] = {};
  set[0] = CMD_SET_CHANNEL; set[1] = 0;
  std::strcpy(reinterpret_cast<char*>(&set[2]), "General");
  for (int i = 0; i < 16; i++) set[2 + 32 + i] = uint8_t(0xB0 + i);
  cmd.handle(set, sizeof(set), out);
  check(out.last()[0] == RESP_CODE_OK, "set channel ok");
  check(f.h.channels_saved, "channels saved");
  out.clear();
  uint8_t get[] = {CMD_GET_CHANNEL, 0};
  cmd.handle(get, 2, out);
  const auto& r = out.last();
  check(r[0] == RESP_CODE_CHANNEL_INFO, "channel info code");
  check(r[1] == 0, "channel idx");
  check(std::strcmp(reinterpret_cast<const char*>(&r[2]), "General") == 0, "channel name");
  check(r[2 + 32] == 0xB0, "channel secret byte0");
}

static void testSendTxtMsgDecrypt() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  // SEND_TXT_MSG: type, attempt, ts(4), prefix(6), text
  uint8_t in[64]; size_t i = 0;
  in[i++] = CMD_SEND_TXT_MSG;
  in[i++] = proto::TXT_TYPE_PLAIN;
  in[i++] = 0;  // attempt
  proto::putU32LE(in, i, 0x12345678); i += 4;
  std::memcpy(&in[i], f.peer.pub_key, 6); i += 6;
  const char* msg = "hello peer";
  std::memcpy(&in[i], msg, std::strlen(msg)); i += std::strlen(msg);
  cmd.handle(in, i, out);
  const auto& r = out.last();
  check(r[0] == RESP_CODE_SENT, "txt sent code");
  check(r[1] == 1, "txt sent flood flag");
  check(proto::getU32LE(r.data(), 6) == f.tx.est, "txt est_timeout");
  check(f.tx.sent.size() == 1, "one packet sent");

  // Decrypt the sent packet as the peer would.
  const proto::Packet& pkt = f.tx.sent[0];
  check(pkt.payloadType() == proto::PAYLOAD_TXT_MSG, "txt payload type");
  check(pkt.payload[0] == f.peer.pub_key[0], "dest hash");
  check(pkt.payload[1] == f.s.self.pub_key[0], "src hash");
  uint8_t secret[proto::PUB_KEY_SIZE];
  f.peer.calcSharedSecret(secret, f.s.self);
  uint8_t dec[256];
  int dlen = proto::MACThenDecrypt(secret, dec, &pkt.payload[2], pkt.payload_len - 2);
  check(dlen > 0, "peer decrypts msg");
  check(proto::getU32LE(dec, 0) == 0x12345678, "msg timestamp");
  check(std::strcmp(reinterpret_cast<char*>(&dec[5]), "hello peer") == 0, "msg text");
}

static void testSendChannelTxt() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  // set channel 0 first
  uint8_t set[2 + 32 + 16] = {}; set[0] = CMD_SET_CHANNEL; set[1] = 0;
  std::strcpy(reinterpret_cast<char*>(&set[2]), "Ch");
  for (int i = 0; i < 16; i++) set[2 + 32 + i] = uint8_t(i + 1);
  cmd.handle(set, sizeof(set), out); out.clear();

  uint8_t in[32]; size_t i = 0;
  in[i++] = CMD_SEND_CHANNEL_TXT_MSG;
  in[i++] = proto::TXT_TYPE_PLAIN;
  in[i++] = 0;  // channel idx
  proto::putU32LE(in, i, 0x9); i += 4;
  const char* t = "hey";
  std::memcpy(&in[i], t, 3); i += 3;
  cmd.handle(in, i, out);
  check(out.last()[0] == RESP_CODE_OK, "channel txt ok");
  check(f.tx.sent.size() == 1 && f.tx.sent[0].payloadType() == proto::PAYLOAD_GRP_TXT, "grp txt sent");
}

static void testSyncQueue() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  // empty -> NO_MORE_MESSAGES
  uint8_t sync[] = {CMD_SYNC_NEXT_MESSAGE};
  cmd.handle(sync, 1, out);
  check(out.last()[0] == RESP_CODE_NO_MORE_MESSAGES, "empty sync");
  // push one via the state and drain
  out.clear();
  uint8_t frame[4] = {RESP_CODE_CONTACT_MSG_RECV, 1, 2, 3};
  f.s.pushOffline(frame, 4);
  cmd.handle(sync, 1, out);
  check(out.last().size() == 4 && out.last()[0] == RESP_CODE_CONTACT_MSG_RECV, "sync returns queued");
  out.clear();
  cmd.handle(sync, 1, out);
  check(out.last()[0] == RESP_CODE_NO_MORE_MESSAGES, "sync empty again");
}

static void testLoginAndReq() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t login[1 + 32 + 6]; login[0] = CMD_SEND_LOGIN;
  std::memcpy(&login[1], f.peer.pub_key, 32);
  std::memcpy(&login[33], "secret", 6);
  cmd.handle(login, sizeof(login), out);
  check(out.last()[0] == RESP_CODE_SENT, "login sent");
  check(f.tx.sent.size() == 1 && f.tx.sent[0].payloadType() == proto::PAYLOAD_ANON_REQ, "login anon_req");

  out.clear(); f.tx.sent.clear();
  uint8_t sreq[1 + 32]; sreq[0] = CMD_SEND_STATUS_REQ; std::memcpy(&sreq[1], f.peer.pub_key, 32);
  cmd.handle(sreq, sizeof(sreq), out);
  check(out.last()[0] == RESP_CODE_SENT, "status req sent");
  check(f.tx.sent[0].payloadType() == proto::PAYLOAD_REQ, "status is REQ");

  // unknown contact
  out.clear();
  uint8_t bad[1 + 32]; bad[0] = CMD_SEND_STATUS_REQ; std::memset(&bad[1], 0x99, 32);
  cmd.handle(bad, sizeof(bad), out);
  check(out.last()[0] == RESP_CODE_ERR && out.last()[1] == ERR_NOT_FOUND, "status req unknown");
}

static void testSigning() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t start[] = {CMD_SIGN_START};
  cmd.handle(start, 1, out);
  check(out.last()[0] == RESP_CODE_SIGN_START, "sign start");
  out.clear();
  uint8_t data[6] = {CMD_SIGN_DATA, 'a','b','c','d','e'};
  cmd.handle(data, 6, out);
  check(out.last()[0] == RESP_CODE_OK, "sign data ok");
  out.clear();
  uint8_t fin[] = {CMD_SIGN_FINISH};
  cmd.handle(fin, 1, out);
  const auto& r = out.last();
  check(r[0] == RESP_CODE_SIGNATURE && r.size() == 1 + proto::SIGNATURE_SIZE, "signature len");
  // verify signature over "abcde"
  check(f.s.self.verify(&r[1], reinterpret_cast<const uint8_t*>("abcde"), 5), "signature verifies");
}

static void testMiscConfig() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t pin[5]; pin[0] = CMD_SET_DEVICE_PIN; proto::putU32LE(pin, 1, 123456);
  cmd.handle(pin, 5, out);
  check(out.last()[0] == RESP_CODE_OK && f.s.ble_pin == 123456, "set pin");
  out.clear();
  uint8_t tun[9]; tun[0] = CMD_SET_TUNING_PARAMS; proto::putU32LE(tun, 1, 100); proto::putU32LE(tun, 5, 200);
  cmd.handle(tun, 9, out);
  check(out.last()[0] == RESP_CODE_OK, "set tuning");
  out.clear();
  uint8_t gt[] = {CMD_GET_TUNING_PARAMS};
  cmd.handle(gt, 1, out);
  check(out.last()[0] == RESP_CODE_TUNING_PARAMS && proto::getU32LE(out.last().data(), 1) == 100, "get tuning");
  out.clear();
  uint8_t aac[] = {CMD_GET_AUTOADD_CONFIG};
  cmd.handle(aac, 1, out);
  check(out.last()[0] == RESP_CODE_AUTOADD_CONFIG, "get autoadd");
  out.clear();
  uint8_t pk[] = {CMD_EXPORT_PRIVATE_KEY};
  cmd.handle(pk, 1, out);
  check(out.last()[0] == RESP_CODE_PRIVATE_KEY && out.last().size() == 65, "export private key");
}

static void testUnknown() {
  Fixture f;
  Collector out; auto cmd = f.handler();
  uint8_t junk[] = {0xEE};
  cmd.handle(junk, 1, out);
  check(out.last()[0] == RESP_CODE_ERR && out.last()[1] == ERR_UNSUPPORTED_CMD, "unknown cmd err");
}

int main() {
  testDeviceQuery();
  testSelfInfo();
  testDeviceTime();
  testSetNameAndTxPower();
  testRadioParams();
  testBattAndStorage();
  testSelfAdvert();
  testContacts();
  testChannels();
  testSendTxtMsgDecrypt();
  testSendChannelTxt();
  testSyncQueue();
  testLoginAndReq();
  testSigning();
  testMiscConfig();
  testUnknown();
  if (g_fail) return 1;
  std::printf("all companion command-handler tests passed\n");
  return 0;
}
