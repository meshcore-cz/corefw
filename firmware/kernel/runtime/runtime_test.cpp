// Host-side kernel runtime tests: packet hash, airtime, duty cycle, and the
// flood router / dispatcher. Uses a fake radio and a virtual clock, so the
// relaying behaviour of a node is verified deterministically on a workstation.
//
//   c++ -std=c++17 -I firmware/kernel/include -I firmware/drivers/crypto/sha256 \
//       firmware/drivers/crypto/sha256/sha256.c \
//       firmware/kernel/runtime/runtime_test.cpp -o /tmp/rtest && /tmp/rtest
#include <corefw/runtime/Airtime.h>
#include <corefw/runtime/Dispatcher.h>

extern "C" {
#include "sha256.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

using namespace corefw;
using proto::Packet;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

namespace {

class VirtualClock : public Clock {
 public:
  uint32_t millis() const override { return t_; }
  void advance(uint32_t dt) { t_ += dt; }
  uint32_t t_ = 0;
};

class FakeRadio : public RadioDriver {
 public:
  bool begin(const RadioConfig&) override { return true; }
  bool configure(const RadioConfig&) override { return true; }
  bool transmit(const uint8_t* d, size_t n) override {
    tx.emplace_back(d, d + n);
    return true;
  }
  void startReceive() override {}
  size_t readReceived(uint8_t* buf, size_t cap) override {
    if (rx.empty()) return 0;
    auto f = rx.front();
    rx.pop_front();
    if (f.size() > cap) return 0;
    std::memcpy(buf, f.data(), f.size());
    return f.size();
  }
  float lastSNR() const override { return 5.0f; }

  void inject(const std::vector<uint8_t>& frame) { rx.push_back(frame); }
  std::deque<std::vector<uint8_t>> rx, tx;
};

class CapturingSink : public PacketSink {
 public:
  bool onPacket(const Packet& p) override {
    delivered++;
    last_payload_len = p.payload_len;
    return false;
  }
  int delivered = 0;
  uint16_t last_payload_len = 0;
};

Packet makeFlood(const char* body) {
  Packet p;
  p.setRouteAndType(proto::ROUTE_FLOOD, proto::PAYLOAD_TXT_MSG);
  p.setPathHashSizeAndCount(1, 0);
  p.payload_len = uint16_t(std::strlen(body));
  std::memcpy(p.payload, body, p.payload_len);
  return p;
}

std::vector<uint8_t> wireOf(const Packet& p) {
  uint8_t buf[proto::MAX_TRANS_UNIT];
  size_t n = p.writeTo(buf);
  return std::vector<uint8_t>(buf, buf + n);
}

}  // namespace

static void testSha256Vector() {
  uint8_t out[32];
  corefw_sha256(reinterpret_cast<const uint8_t*>("abc"), 3, out);
  // FIPS 180-4 known answer.
  const uint8_t want[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                            0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                            0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  check(std::memcmp(out, want, 32) == 0, "SHA-256(\"abc\") matches FIPS vector");
}

static void testAirtime() {
  // SF11/BW250 is heavier than SF7/BW250; longer payload => more airtime.
  uint32_t a = timeOnAirMs(20, 250.0f, 7);
  uint32_t b = timeOnAirMs(20, 250.0f, 11);
  uint32_t c = timeOnAirMs(60, 250.0f, 11);
  check(b > a, "higher SF => more airtime");
  check(c > b, "larger payload => more airtime");
  check(a > 0 && c < 5000, "airtime in a sane range");
}

static void testDutyCycle() {
  DutyCycleLimiter d(0.01f);
  check(d.allows(0), "initially allowed");
  d.record(1000, 100);  // 100ms airtime at 1% => 10s gap
  check(!d.allows(1000), "blocked right after tx");
  check(!d.allows(5000), "still blocked mid-gap");
  check(d.allows(1000 + 10000), "allowed after off-time");
}

static void testFloodForwardAndDedup() {
  VirtualClock clk;
  FakeRadio radio;
  uint8_t bpub[proto::PUB_KEY_SIZE];
  for (size_t i = 0; i < sizeof(bpub); i++) bpub[i] = uint8_t(0xB0 + i);
  RadioConfig cfg;  // defaults: SF11/BW250
  Dispatcher disp(&radio, &clk, bpub, cfg);
  disp.setDuty(1.0f);  // remove duty gating for routing assertions
  CapturingSink sink;
  disp.subscribe(&sink);

  Packet original = makeFlood("hello mesh");
  radio.inject(wireOf(original));

  disp.loop();  // receive + deliver + queue forward
  check(sink.delivered == 1, "packet delivered to sink");
  check(disp.queueDepth() == 1, "forward queued");

  clk.advance(2000);  // past the retransmit delay (airtime * priority)
  disp.loop();        // transmit the forward
  check(radio.tx.size() == 1, "forwarded frame transmitted");

  // The forwarded frame must carry B's hash appended to the path.
  Packet fwd;
  check(fwd.readFrom(radio.tx.front().data(), radio.tx.front().size()), "parse forwarded");
  check(fwd.pathHashCount() == 1, "path hash count incremented");
  check(fwd.path[0] == bpub[0], "our hash appended to path");
  check(fwd.payload_len == original.payload_len, "payload preserved");

  // Re-injecting the ORIGINAL (path count 0) is a duplicate: the packet hash is
  // over type+payload, so path changes don't defeat dedup.
  radio.inject(wireOf(original));
  disp.loop();
  check(disp.duplicates() == 1, "original re-seen is a duplicate");

  // Re-injecting B's own forwarded frame is also a duplicate (echo suppression).
  radio.inject(std::vector<uint8_t>(radio.tx.front()));
  disp.loop();
  check(disp.duplicates() == 2, "forwarded echo suppressed");
  check(radio.tx.size() == 1, "no re-transmit of duplicates");
}

static void testDutyGatingHoldsQueue() {
  VirtualClock clk;
  FakeRadio radio;
  uint8_t pub[proto::PUB_KEY_SIZE] = {0};
  RadioConfig cfg;
  Dispatcher disp(&radio, &clk, pub, cfg);
  disp.setDuty(0.01f);

  disp.send(makeFlood("one"));
  disp.loop();
  check(radio.tx.size() == 1, "first send transmits");

  disp.send(makeFlood("two"));
  disp.loop();
  check(radio.tx.size() == 1, "second send held by duty cycle");

  clk.advance(60000);  // well past the off-time
  disp.loop();
  check(radio.tx.size() == 2, "held packet transmits after off-time");
}

int main() {
  testSha256Vector();
  testAirtime();
  testDutyCycle();
  testFloodForwardAndDedup();
  testDutyGatingHoldsQueue();
  std::printf("all kernel runtime tests passed\n");
  return 0;
}
