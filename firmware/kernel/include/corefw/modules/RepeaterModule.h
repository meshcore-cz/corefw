// RepeaterModule — the store-and-forward repeater role.
//
// It subscribes to received packets and lets the kernel's routing mechanism
// rebroadcast floods; it periodically self-advertises. Radio access is only via
// the MeshService — the module never transmits directly.
#pragma once

#include <corefw/Mesh.h>
#include <corefw/Module.h>
#include <corefw/protocol/AdvertData.h>

namespace corefw {

class RepeaterModule : public Module, public PacketSink {
 public:
  const char* name() const override { return "repeater"; }

  // Configuration setters, driven by generated code from validated options.
  void setAdvertName(const char* n) { advert_name_ = n; }
  void setAdminPassword(const char* p) { admin_password_ = p; }
  void setMaxNeighbours(int n) { max_neighbours_ = n; }
  void setAdvertInterval(const char* s) { advert_interval_s_ = parseDurationSeconds(s); }

  void initialize(Context& ctx) override {
    ctx_ = &ctx;
    ctx.mesh().subscribe(this);
  }

  void start() override {
    // The kernel scheduler will call us back to emit the first advert; details
    // live in the kernel's routing/scheduling mechanism.
  }

  // PacketSink: the kernel's router handles forwarding; a repeater has no
  // local-delivery behaviour, so it consumes nothing.
  bool onPacket(const proto::Packet&) override { return false; }

  const char* advertName() const { return advert_name_; }
  int maxNeighbours() const { return max_neighbours_; }
  uint32_t advertIntervalSeconds() const { return advert_interval_s_; }

 private:
  static uint32_t parseDurationSeconds(const char* s) {
    if (!s) return 0;
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9') {
      n = n * 10u + uint32_t(*s - '0');
      s++;
    }
    while (*s == ' ') s++;
    if (s[0] == 'd') return n * 24u * 60u * 60u;
    if (s[0] == 'h') return n * 60u * 60u;
    if (s[0] == 'm') return n * 60u;
    return n;
  }

  Context* ctx_ = nullptr;
  const char* advert_name_ = "CoreFW Repeater";
  const char* admin_password_ = "password";
  int max_neighbours_ = 50;
  uint32_t advert_interval_s_ = 15u * 60u;
};

}  // namespace corefw
