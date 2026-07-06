// SimplePowerPolicy — baseline power behaviour.
//
// A minimal but real policy: below the low threshold it asks the coordinator to
// prefer sleep; below the critical threshold it forces deep sleep to protect a
// reserve. tx_power is applied to the radio at startup by the kernel.
#pragma once

#include <corefw/Power.h>

namespace corefw {

class SimplePowerPolicy : public PowerPolicy {
 public:
  void setLowBatteryThreshold(int percent) override { low_ = percent; }
  void setCriticalBatteryThreshold(int percent) override { critical_ = percent; }
  void setTxPower(int dbm) override { tx_power_ = dbm; }

  int lowBatteryThreshold() const { return low_; }
  int criticalBatteryThreshold() const { return critical_; }
  int txPower() const { return tx_power_; }

  void evaluate(int battery_percent, bool external_power,
                PowerCoordinator& power) override {
    if (external_power) {
      power.preventDeepSleep("external power");
      return;
    }
    power.releaseDeepSleep("external power");
    if (battery_percent <= critical_) {
      // Deep sleep for an hour to protect the reserve.
      power.scheduleWake(3600ull * 1000);
    }
  }

 private:
  int low_ = 30;
  int critical_ = 15;
  int tx_power_ = 22;
};

}  // namespace corefw
