// Host-side kernel lifecycle smoke test.
//
// Registers a fake module and drives Kernel::begin with fake services, checking
// the configure -> initialize -> start ordering and that registration wiring
// works. Build & run:
//
//   c++ -std=c++17 -I firmware/kernel/include \
//       firmware/kernel/Kernel.cpp firmware/kernel/kernel_test.cpp \
//       -o /tmp/ktest && /tmp/ktest
#include <corefw/Kernel.h>
#include <corefw/policies/SimplePowerPolicy.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace corefw;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

namespace {

std::string trace;  // records lifecycle order

class FakeMesh : public MeshService {
 public:
  void send(const proto::Packet&) override {}
  void subscribe(PacketSink*) override { subscribed = true; }
  const uint8_t* selfHash() const override { return hash; }
  bool subscribed = false;
  uint8_t hash[1] = {0};
};

class FakePower : public PowerCoordinator {
 public:
  void requireRadioUntil(uint64_t) override {}
  void scheduleWake(uint64_t) override { woke = true; }
  void preventDeepSleep(const char*) override {}
  void releaseDeepSleep(const char*) override {}
  bool woke = false;
};

class FakeModule : public Module {
 public:
  const char* name() const override { return "fake"; }
  void configure() override { trace += "C"; }
  void initialize(Context& ctx) override { trace += "I"; ctx.mesh().subscribe(&sink); }
  void start() override { trace += "S"; }

  class Sink : public PacketSink {
   public:
    bool onPacket(const proto::Packet&) override { return true; }
  } sink;
};

}  // namespace

int main() {
  Kernel kernel;
  FakeModule mod;
  SimplePowerPolicy policy;
  policy.setLowBatteryThreshold(30);
  policy.setCriticalBatteryThreshold(15);

  kernel.registerModule(&mod);
  kernel.setPowerPolicy(&policy);
  check(kernel.moduleCount() == 1, "module registered");
  check(kernel.powerPolicy() == &policy, "policy registered");

  FakeMesh mesh;
  FakePower power;
  kernel.begin(mesh, power);

  check(trace == "CIS", "lifecycle order configure->initialize->start");
  check(mesh.subscribed, "module subscribed via context");

  // Event dispatch reaches modules.
  Event e{EventType::BatteryChanged, nullptr, 42};
  kernel.dispatch(e);

  std::printf("kernel lifecycle test passed\n");
  return 0;
}
