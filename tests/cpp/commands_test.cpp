// Host-side Companion Protocol command-handler tests.
//
// Feeds inbound command frames and asserts the exact response bytes, matching
// the reference companion firmware. Build & run:
//
//   c++ -std=c++17 -I firmware/kernel/include \
//       firmware/kernel/companion/commands_test.cpp -o /tmp/cmdtest && /tmp/cmdtest
#include <corefw/companion/Commands.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace corefw::companion;
namespace proto = corefw::proto;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

namespace {
class FakeHost : public CompanionHost {
 public:
  uint32_t rtcNow() override { return now; }
  void setRtc(uint32_t s) override { now = s; }
  uint16_t batteryMilliVolts() override { return 3700; }
  uint32_t storageUsedKb() override { return 12; }
  uint32_t storageTotalKb() override { return 1024; }
  void sendSelfAdvert(bool flood) override {
    advert_sent = true;
    advert_flood = flood;
  }
  const char* manufacturerName() override { return "Seeed Studio"; }

  uint32_t now = 1000000;
  bool advert_sent = false;
  bool advert_flood = false;
};
}  // namespace

static void testDeviceQuery() {
  CompanionState s;
  s.ble_pin = 0x00010203;
  FakeHost h;
  CommandHandler cmd(s, h);
  uint8_t in[] = {CMD_DEVICE_QUERY, 0x0D};  // app protocol ver 13
  uint8_t out[MAX_FRAME_SIZE];
  size_t n = cmd.handle(in, sizeof(in), out);
  check(out[0] == RESP_CODE_DEVICE_INFO, "device_info code");
  check(out[1] == FIRMWARE_VER_CODE, "firmware ver code 13");
  check(out[2] == s.max_contacts / 2, "max contacts/2");
  check(out[3] == s.max_group_channels, "max group channels");
  check(proto::getU32LE(out, 4) == s.ble_pin, "ble pin LE");
  check(n > 80, "device_info frame size");
}

static void testAppStartSelfInfo() {
  CompanionState s;
  for (int i = 0; i < 32; i++) s.self_pub[i] = uint8_t(i);
  std::strcpy(s.node_name, "Wio Companion");
  s.tx_power_dbm = 20;
  s.freq_khz = 869525;
  s.bw_hz = 250000;
  s.sf = 11;
  FakeHost h;
  CommandHandler cmd(s, h);
  // CMD_APP_START requires len >= 8 (1 cmd + 6 reserved + app name).
  uint8_t in[16] = {CMD_APP_START, 0, 0, 0, 0, 0, 0, 0, 'a', 'p', 'p'};
  uint8_t out[MAX_FRAME_SIZE];
  size_t n = cmd.handle(in, 11, out);
  check(out[0] == RESP_CODE_SELF_INFO, "self_info code");
  check(out[1] == ADV_TYPE_CHAT, "adv type chat");
  check(out[2] == 20, "tx power");
  check(std::memcmp(&out[4], s.self_pub, 32) == 0, "pub key echoed");
  // After code(1)+advtype(1)+tx(1)+maxtx(1)+pub(32)+lat(4)+lon(4)+multiacks(1)
  //   +locpolicy(1)+telemetry(1)+manualadd(1) = 48, then freq(4).
  check(proto::getU32LE(out, 48) == 869525, "freq kHz LE");
  check(proto::getU32LE(out, 52) == 250000, "bw Hz LE");
  check(out[56] == 11, "sf");
  // node name at the tail.
  check(std::memcmp(&out[58], "Wio Companion", 13) == 0, "node name tail");
  check(n == 58 + 13, "self_info length");
}

static void testTime() {
  CompanionState s;
  FakeHost h;
  h.now = 500;
  CommandHandler cmd(s, h);
  uint8_t out[MAX_FRAME_SIZE];

  uint8_t get[] = {CMD_GET_DEVICE_TIME};
  size_t n = cmd.handle(get, 1, out);
  check(n == 5 && out[0] == RESP_CODE_CURR_TIME, "curr_time code");
  check(proto::getU32LE(out, 1) == 500, "curr time value");

  uint8_t set[5] = {CMD_SET_DEVICE_TIME};
  proto::putU32LE(set, 1, 1000);
  check(cmd.handle(set, 5, out) == 1 && out[0] == RESP_CODE_OK, "set future time ok");
  check(h.now == 1000, "rtc updated");

  uint8_t setPast[5] = {CMD_SET_DEVICE_TIME};
  proto::putU32LE(setPast, 1, 10);
  check(out[0] == RESP_CODE_OK, "");  // reset out
  size_t m = cmd.handle(setPast, 5, out);
  check(m == 2 && out[0] == RESP_CODE_ERR && out[1] == ERR_ILLEGAL_ARG, "set past time rejected");
}

static void testNameLatLonTxAdvert() {
  CompanionState s;
  FakeHost h;
  CommandHandler cmd(s, h);
  uint8_t out[MAX_FRAME_SIZE];

  uint8_t name[] = {CMD_SET_ADVERT_NAME, 'N', 'o', 'd', 'e'};
  check(cmd.handle(name, sizeof(name), out) == 1 && out[0] == RESP_CODE_OK, "set name ok");
  check(std::strcmp(s.node_name, "Node") == 0, "name stored");

  uint8_t ll[9] = {CMD_SET_ADVERT_LATLON};
  proto::putU32LE(ll, 1, uint32_t(50080000));   // 50.08
  proto::putU32LE(ll, 5, uint32_t(14420000));   // 14.42
  check(cmd.handle(ll, 9, out) == 1 && out[0] == RESP_CODE_OK, "set latlon ok");
  check(s.lat_e6 == 50080000 && s.lon_e6 == 14420000, "latlon stored");

  uint8_t bad[9] = {CMD_SET_ADVERT_LATLON};
  proto::putU32LE(bad, 1, uint32_t(int32_t(-95000000)));  // out of range
  proto::putU32LE(bad, 5, 0);
  check(cmd.handle(bad, 9, out) == 2 && out[1] == ERR_ILLEGAL_ARG, "bad latlon rejected");

  uint8_t tx[] = {CMD_SET_RADIO_TX_POWER, 40};  // above max (22)
  check(cmd.handle(tx, 2, out) == 1 && out[0] == RESP_CODE_OK, "set tx ok");
  check(s.tx_power_dbm == s.max_tx_power_dbm, "tx power capped to max");

  uint8_t adv[] = {CMD_SEND_SELF_ADVERT, 1};  // flood
  check(cmd.handle(adv, 2, out) == 1 && out[0] == RESP_CODE_OK, "send advert ok");
  check(h.advert_sent && h.advert_flood, "advert sent as flood");
}

static void testBattAndUnknown() {
  CompanionState s;
  FakeHost h;
  CommandHandler cmd(s, h);
  uint8_t out[MAX_FRAME_SIZE];

  uint8_t batt[] = {CMD_GET_BATT_AND_STORAGE};
  size_t n = cmd.handle(batt, 1, out);
  check(n == 11 && out[0] == RESP_CODE_BATT_AND_STORAGE, "batt code");
  check(proto::getU16LE(out, 1) == 3700, "battery mv");
  check(proto::getU32LE(out, 3) == 12, "storage used");
  check(proto::getU32LE(out, 7) == 1024, "storage total");

  uint8_t junk[] = {0xEE};
  check(cmd.handle(junk, 1, out) == 2 && out[0] == RESP_CODE_ERR && out[1] == ERR_UNSUPPORTED_CMD,
        "unknown cmd rejected");
}

int main() {
  testDeviceQuery();
  testAppStartSelfInfo();
  testTime();
  testNameLatLonTxAdvert();
  testBattAndUnknown();
  std::printf("all companion command-handler tests passed\n");
  return 0;
}
