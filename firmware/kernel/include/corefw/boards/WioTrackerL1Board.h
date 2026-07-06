// WioTrackerL1Board — board package for the Seeed Wio Tracker L1.
//
// nRF52840 + SX1262, SH1106 OLED, on-board GPS. Pin assignments arrive as -D
// defines emitted by corefw from components/boards/wio-tracker-l1/component.yaml.
#pragma once

#include <corefw/Board.h>

namespace corefw {

class WioTrackerL1Board : public Board {
 public:
  const char* manufacturerName() const override { return "Seeed Studio"; }
  const char* boardName() const override { return "Wio Tracker L1"; }

  BoardCapabilities capabilities() const override {
    BoardCapabilities c;
    c.radio = c.display = c.battery = c.gps = true;
    c.bluetooth = c.usb = c.deep_sleep = true;
    c.wifi = false;
    return c;
  }

  int maxTxPowerDbm() const override { return 22; }

  void begin() override { /* bring up buses, GPS rail, radio */ }
  RadioDriver* radio() override { return radio_; }
  void reboot() override { /* NVIC_SystemReset() on target */ }

 private:
  RadioDriver* radio_ = nullptr;
};

}  // namespace corefw
