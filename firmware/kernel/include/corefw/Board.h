// The Board API — the stable contract a board package implements.
//
// A board package describes how hardware components are wired on a particular
// device and mainly assembles reusable drivers (radio, PMU, display, GPS). It
// implements hardware mechanisms but does not decide device policy.
#pragma once

#include <cstdint>

namespace corefw {

class RadioDriver;  // firmware/drivers/radio

// Capabilities a board may advertise. Modules/policies query these instead of
// hard-coding board knowledge.
struct BoardCapabilities {
  bool radio = false;
  bool display = false;
  bool battery = false;
  bool gps = false;
  bool bluetooth = false;
  bool wifi = false;
  bool usb = false;
  bool deep_sleep = false;
};

class Board {
 public:
  virtual ~Board() = default;

  // Identity / metadata.
  virtual const char* manufacturerName() const = 0;
  virtual const char* boardName() const = 0;
  virtual BoardCapabilities capabilities() const = 0;
  virtual int maxTxPowerDbm() const = 0;

  // Lifecycle.
  virtual void begin() = 0;                 // bring up buses, power rails
  virtual RadioDriver* radio() = 0;         // the board's radio driver, or null

  // Power / battery.
  virtual uint16_t batteryMilliVolts() { return 0; }
  virtual bool isExternalPowered() { return false; }

  // Sleep is mediated by the kernel/PowerCoordinator; a board only implements
  // the mechanism.
  virtual void deepSleep(uint32_t seconds) { (void)seconds; }
  virtual void reboot() = 0;
};

}  // namespace corefw
