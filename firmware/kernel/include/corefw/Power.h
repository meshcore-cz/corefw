// Central power coordination.
//
// Unlike a generic component system, corefw has battery-wide constraints:
// multiple components can have conflicting power requirements. Modules never
// enter deep sleep themselves — they submit requirements to the PowerCoordinator
// (keep the radio up until T, wake me at T2, hold sleep off while USB is
// active), and the coordinator decides the actual sleep/wake schedule.
#pragma once

#include <cstdint>

namespace corefw {

// PowerCoordinator collects requirements from modules and the active policy.
class PowerCoordinator {
 public:
  virtual ~PowerCoordinator() = default;

  // Keep the radio powered until at least `epoch_ms`.
  virtual void requireRadioUntil(uint64_t epoch_ms) = 0;
  // Request a wake at `epoch_ms` (the earliest requested wake wins).
  virtual void scheduleWake(uint64_t epoch_ms) = 0;
  // Prevent deep sleep while `reason` is held; call releaseDeepSleep to clear.
  virtual void preventDeepSleep(const char* reason) = 0;
  virtual void releaseDeepSleep(const char* reason) = 0;
};

// PowerPolicy decides the device's high-level power behaviour from battery and
// power-source state. Exactly one policy is active per device. The setters here
// are the stable Policy API surface that generated code drives from validated
// configuration (see components/policies/*/component.yaml `codegen.setters`).
class PowerPolicy {
 public:
  virtual ~PowerPolicy() = default;

  virtual void setLowBatteryThreshold(int percent) = 0;
  virtual void setCriticalBatteryThreshold(int percent) = 0;
  virtual void setTxPower(int dbm) { (void)dbm; }

  // evaluate is called by the kernel with the current battery percentage and
  // external-power flag; the policy pushes its decisions into the coordinator.
  virtual void evaluate(int battery_percent, bool external_power,
                        PowerCoordinator& power) = 0;
};

}  // namespace corefw
