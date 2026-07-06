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

 private:
  Context* ctx_ = nullptr;
  const char* advert_name_ = "CoreFW Repeater";
  const char* admin_password_ = "password";
  int max_neighbours_ = 50;
};

}  // namespace corefw
