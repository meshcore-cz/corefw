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

  // Idle the MCU until the next interrupt (radio IRQ, BLE/USB, timer) or up to
  // `max_ms`, whichever comes first, to cut idle current without dropping any
  // wake source. The main loop calls this when there is no pending work. Default
  // is a no-op (host builds and boards without a low-power idle). `max_ms == 0`
  // means "until the next event" (event-driven idle). This is a light idle, NOT
  // deep sleep: RAM, the radio receiver, and any BLE/USB link stay live.
  virtual void lightSleep(uint32_t max_ms) { (void)max_ms; }

  virtual void reboot() = 0;
};

}  // namespace corefw
