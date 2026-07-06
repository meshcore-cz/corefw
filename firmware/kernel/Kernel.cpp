// Kernel runtime.
//
// This is the portable core of the kernel's startup sequence. Platform-specific
// setup() (Arduino/ESP-IDF) constructs the concrete MeshService and
// PowerCoordinator, calls corefw_compose() to register the profile's board,
// modules and policies, then calls Kernel::begin() to drive them to life.
#include <corefw/Kernel.h>

namespace corefw {

void Kernel::begin(MeshService& mesh, PowerCoordinator& power) {
  // 1. Bring up hardware via the board package.
  if (board_ != nullptr) {
    board_->begin();
  }

  // 2. Build the context modules use to reach stable services.
  static Context ctx(*this, mesh, power);

  // 3. Drive each module through the deterministic lifecycle:
  //    configure -> initialize -> start.
  for (int i = 0; i < module_count_; ++i) {
    modules_[i]->configure();
  }
  for (int i = 0; i < module_count_; ++i) {
    modules_[i]->initialize(ctx);
  }
  for (int i = 0; i < module_count_; ++i) {
    modules_[i]->start();
  }

  // 4. Prime the power policy with an initial evaluation, if present.
  if (power_policy_ != nullptr && board_ != nullptr) {
    const bool external = board_->isExternalPowered();
    power_policy_->evaluate(/*battery_percent=*/100, external, power);
  }
}

}  // namespace corefw
