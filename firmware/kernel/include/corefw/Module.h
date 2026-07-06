// The Module API — device roles and services (repeater, companion, sensor, ...).
//
// Modules are the composable units that define what a device does. They consume
// stable kernel services through the Context and react to events; they must not
// touch the radio or sleep directly. The lifecycle is designed around radio and
// power constraints rather than a busy main loop.
#pragma once

#include <corefw/Event.h>

namespace corefw {

namespace proto { struct AdvertData; }

class Kernel;
class MeshService;
class PowerCoordinator;

// Context is the module's window onto the kernel's stable services. A module
// requests a transmission via mesh().send(...); it is never handed the raw
// radio, so the kernel stays responsible for when a transmission is safe.
class Context {
 public:
  Context(Kernel& kernel, MeshService& mesh, PowerCoordinator& power)
      : kernel_(kernel), mesh_(mesh), power_(power) {}

  Kernel& kernel() { return kernel_; }
  MeshService& mesh() { return mesh_; }
  PowerCoordinator& power() { return power_; }

 private:
  Kernel& kernel_;
  MeshService& mesh_;
  PowerCoordinator& power_;
};

class Module {
 public:
  virtual ~Module() = default;

  // Human-readable id for diagnostics.
  virtual const char* name() const = 0;

  // Lifecycle, called by the kernel in this order:
  virtual void configure() {}                       // apply validated config
  virtual void initialize(Context& ctx) { (void)ctx; }  // acquire services
  virtual void start() {}                            // begin operating
  virtual void onEvent(const Event& e) { (void)e; }  // react to kernel events

  // Power transitions.
  virtual void prepareForSleep() {}
  virtual void afterWake() {}

  virtual void shutdown() {}

  // Extension hook: contribute to a self-advert before it is signed. The kernel
  // calls this on every registered module (via Kernel::applyAdvertDecorators)
  // when building an advert, so optional extension components — e.g. an advert
  // feature-flag extension — can enrich the advert without any core changes or
  // a fork. The default is a no-op.
  virtual void decorateAdvert(proto::AdvertData&) {}
};

}  // namespace corefw
