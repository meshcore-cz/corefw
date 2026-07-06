// Host test for the storage codecs. Asserts exact byte offsets against the
// reference DataStore record layouts (the flash-compatibility contract) plus
// round-trips. If these offsets drift, a corefw flash would misread an existing
// MeshCore device's stored data.
#include <corefw/companion/Storage.h>
#include <corefw/companion/StorageCodec.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace corefw::companion;
namespace proto = corefw::proto;

static int g_fail = 0;
static void check(bool c, const char* w) { if (!c) { std::printf("FAIL: %s\n", w); g_fail = 1; } }

static void testPrefsOffsets() {
  CompanionState s;
  std::strcpy(s.node_name, "MyNode");
  s.freq_khz = 869525;   // 869.525 MHz
  s.bw_hz = 250000;      // 250 kHz
  s.sf = 11; s.cr = 5;
  s.tx_power_dbm = 22;
  s.ble_pin = 0x000F4240;  // 1000000
  s.lat_e6 = 50123456; s.lon_e6 = 14456789;
  s.advert_loc_policy = 1;
  s.multi_acks = 1;
  s.path_hash_mode = 2;
  s.autoadd_config = 3;
  s.autoadd_max_hops = 4;
  std::strcpy(s.default_scope_name, "scope");
  for (int i = 0; i < 16; i++) s.default_scope_key[i] = uint8_t(i);

  uint8_t rec[PREFS_RECORD_SIZE];
  encodePrefs(s, rec);

  check(std::strcmp(reinterpret_cast<char*>(&rec[4]), "MyNode") == 0, "prefs node_name @4");
  check(getFloat(&rec[56]) == 869.525f, "prefs freq @56 MHz");
  check(rec[60] == 11 && rec[61] == 5, "prefs sf/cr @60/61");
  check(getFloat(&rec[64]) == 250.0f, "prefs bw @64 kHz");
  check(int8_t(rec[68]) == 22, "prefs tx_power @68");
  check(rec[76] == 1, "prefs advert_loc @76");
  check(rec[77] == 1, "prefs multi_acks @77");
  check(rec[78] == 2, "prefs path_hash_mode @78");
  check(proto::getU32LE(rec, 80) == 1000000, "prefs ble_pin @80");
  check(rec[87] == 3, "prefs autoadd_config @87");
  check(rec[88] == 4, "prefs autoadd_max_hops @88");
  check(std::strcmp(reinterpret_cast<char*>(&rec[90]), "scope") == 0, "prefs scope_name @90");
  check(rec[121] == 0 && rec[121 + 15] == 15, "prefs scope_key @121");
  check(getDouble(&rec[40]) > 50.12 && getDouble(&rec[40]) < 50.13, "prefs lat @40");

  // Round-trip.
  CompanionState s2;
  decodePrefs(rec, s2);
  check(std::strcmp(s2.node_name, "MyNode") == 0, "prefs rt name");
  check(s2.freq_khz == 869525, "prefs rt freq");
  check(s2.bw_hz == 250000, "prefs rt bw");
  check(s2.sf == 11 && s2.cr == 5, "prefs rt sf/cr");
  check(s2.tx_power_dbm == 22, "prefs rt txp");
  check(s2.ble_pin == 1000000, "prefs rt pin");
  check(s2.autoadd_config == 3 && s2.autoadd_max_hops == 4, "prefs rt autoadd");
  check(s2.lat_e6 == 50123456, "prefs rt lat");
  check(std::memcmp(s2.default_scope_key, s.default_scope_key, 16) == 0, "prefs rt scope key");
}

static void testContactRecord() {
  ContactInfo c;
  for (int i = 0; i < 32; i++) c.id.pub_key[i] = uint8_t(0x40 + i);
  std::strcpy(c.name, "Alice");
  c.type = 1; c.flags = 2;
  c.sync_since = 111;
  c.out_path_len = 3;
  c.last_advert_timestamp = 222;
  for (int i = 0; i < 3; i++) c.out_path[i] = uint8_t(0x90 + i);
  c.lastmod = 333;
  c.gps_lat = 50000000; c.gps_lon = -14000000;

  uint8_t rec[CONTACT_RECORD_SIZE];
  encodeContact(c, rec);
  // Offsets: pub(32) name(32) type(64) flags(65) unused(66) sync(67) opl(71)
  //          last_advert(72) out_path(76) lastmod(140) lat(144) lon(148)
  check(rec[64] == 1, "contact type @64");
  check(rec[65] == 2, "contact flags @65");
  check(proto::getU32LE(rec, 67) == 111, "contact sync_since @67");
  check(rec[71] == 3, "contact out_path_len @71");
  check(proto::getU32LE(rec, 72) == 222, "contact last_advert @72");
  check(rec[76] == 0x90, "contact out_path @76");
  check(proto::getU32LE(rec, 140) == 333, "contact lastmod @140");
  check(int32_t(proto::getU32LE(rec, 144)) == 50000000, "contact lat @144");
  check(int32_t(proto::getU32LE(rec, 148)) == -14000000, "contact lon @148");

  ContactInfo c2;
  decodeContact(rec, c2);
  check(std::memcmp(c2.id.pub_key, c.id.pub_key, 32) == 0, "contact rt pubkey");
  check(std::strcmp(c2.name, "Alice") == 0, "contact rt name");
  check(c2.type == 1 && c2.flags == 2 && c2.out_path_len == 3, "contact rt fields");
  check(c2.sync_since == 111 && c2.lastmod == 333, "contact rt timestamps");
  check(c2.gps_lat == 50000000 && c2.gps_lon == -14000000, "contact rt gps");
}

static void testChannelRecord() {
  ChannelDetails ch;
  std::strcpy(ch.name, "General");
  uint8_t key[16];
  for (int i = 0; i < 16; i++) key[i] = uint8_t(0xC0 + i);
  ch.channel.setSecret(key);

  uint8_t rec[CHANNEL_RECORD_SIZE];
  encodeChannel(ch, rec);
  check(std::strcmp(reinterpret_cast<char*>(&rec[4]), "General") == 0, "channel name @4");
  check(rec[36] == 0xC0, "channel secret @36");
  check(rec[36 + 16] == 0, "channel upper secret zero @52");  // 256-bit half unused

  ChannelDetails ch2;
  decodeChannel(rec, ch2);
  check(std::strcmp(ch2.name, "General") == 0, "channel rt name");
  check(std::memcmp(ch2.channel.secret, key, 16) == 0, "channel rt secret");
  // hash recomputed from secret
  uint8_t digest[32]; corefw_sha256(key, 16, digest);
  check(ch2.channel.hash[0] == digest[0], "channel rt hash");
}

static void testIdentityRecord() {
  uint8_t seed[proto::SEED_SIZE]; std::memset(seed, 0x55, sizeof(seed));
  proto::LocalIdentity id = proto::LocalIdentity::fromSeed(seed);
  uint8_t rec[IDENTITY_RECORD_SIZE];
  encodeIdentity(id, "NodeName", rec);
  check(std::memcmp(rec, id.prv_key, proto::PRV_KEY_SIZE) == 0, "identity prv first");
  check(std::memcmp(&rec[proto::PRV_KEY_SIZE], id.pub_key, proto::PUB_KEY_SIZE) == 0, "identity pub next");
  check(std::strcmp(reinterpret_cast<char*>(&rec[96]), "NodeName") == 0, "identity name @96");

  proto::LocalIdentity id2; char name[32];
  decodeIdentity(rec, IDENTITY_RECORD_SIZE, id2, name, sizeof(name));
  check(std::memcmp(id2.pub_key, id.pub_key, proto::PUB_KEY_SIZE) == 0, "identity rt pub");
  check(std::memcmp(id2.prv_key, id.prv_key, proto::PRV_KEY_SIZE) == 0, "identity rt prv");
  check(std::strcmp(name, "NodeName") == 0, "identity rt name");
}

// In-memory FileStore for exercising PersistentStore end to end.
class MemFS : public FileStore {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  bool exists(const char* p) override { return files.count(p) > 0; }
  size_t read(const char* p, uint8_t* buf, size_t cap) override {
    auto it = files.find(p);
    if (it == files.end()) return 0;
    size_t n = it->second.size(); if (n > cap) n = cap;
    std::memcpy(buf, it->second.data(), n);
    return n;
  }
  bool write(const char* p, const uint8_t* d, size_t len) override {
    files[p].assign(d, d + len); return true;
  }
  bool remove(const char* p) override { return files.erase(p) > 0; }
};

static void testPersistentStoreRoundTrip() {
  MemFS fs;
  PersistentStore store(fs);

  // First boot: no identity -> generate + persist, then prefs/contacts/channels.
  CompanionState s;
  std::strcpy(s.node_name, "Wio");
  uint8_t seed[proto::SEED_SIZE]; std::memset(seed, 0x77, sizeof(seed));
  store.loadAll(s, seed);
  check(fs.exists("/_main.id"), "identity persisted on first boot");

  // Populate and save.
  s.freq_khz = 868000; s.ble_pin = 654321; s.tx_power_dbm = 20;
  ContactInfo c; std::memset(c.id.pub_key, 0xAB, 32); std::strcpy(c.name, "Bob");
  c.type = ADV_TYPE_CHAT; c.lastmod = 42; s.addContact(c);
  ChannelDetails ch; std::strcpy(ch.name, "Ch"); uint8_t k[16]; std::memset(k, 0x5, 16);
  ch.channel.setSecret(k); s.setChannel(0, ch);
  store.savePrefs(s);
  store.saveContacts(s);
  store.saveChannels(s);

  // Reboot: fresh state, same FS -> everything restored, identity preserved.
  CompanionState s2;
  store.loadAll(s2, seed);  // seed unused since identity now exists
  check(std::memcmp(s2.self.pub_key, s.self.pub_key, proto::PUB_KEY_SIZE) == 0, "identity preserved across reboot");
  check(s2.freq_khz == 868000 && s2.ble_pin == 654321 && s2.tx_power_dbm == 20, "prefs restored");
  check(s2.num_contacts == 1 && std::strcmp(s2.contacts[0].name, "Bob") == 0, "contact restored");
  check(s2.channel_used[0] && std::strcmp(s2.channels[0].name, "Ch") == 0, "channel restored");
  check(std::memcmp(s2.channels[0].channel.secret, k, 16) == 0, "channel secret restored");

  // Anon contacts are not persisted.
  ContactInfo anon; std::memset(anon.id.pub_key, 0x01, 32); anon.type = ADV_TYPE_NONE;
  s2.addContact(anon);
  store.saveContacts(s2);
  CompanionState s3; store.loadAll(s3, seed);
  check(s3.num_contacts == 1, "anon contact not persisted");
}

int main() {
  testPrefsOffsets();
  testContactRecord();
  testChannelRecord();
  testIdentityRecord();
  testPersistentStoreRoundTrip();
  if (g_fail) return 1;
  std::printf("all storage codec tests passed\n");
  return 0;
}
