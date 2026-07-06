// HeltecV3Board — board package for the Heltec WiFi LoRa 32 V3.
//
// ESP32-S3 + SX1262, SSD1306 OLED. Pin assignments arrive as -D defines emitted
// by corefw from components/boards/heltec-v3/component.yaml, so the wiring lives
// in declarative config and this class assembles the drivers.
#pragma once

#include <corefw/Board.h>

namespace corefw {

class HeltecV3Board : public Board {
 public:
  const char* manufacturerName() const override { return "Heltec"; }
  const char* boardName() const override { return "Heltec V3"; }

  BoardCapabilities capabilities() const override {
    BoardCapabilities c;
    c.radio = c.display = c.battery = true;
    c.bluetooth = c.wifi = c.usb = c.gps = c.deep_sleep = true;
    return c;
  }

  int maxTxPowerDbm() const override { return 22; }

  void begin() override { /* bring up VEXT rail, I2C/SPI buses, radio */ }
  RadioDriver* radio() override { return radio_; }
  void reboot() override { /* esp_restart() on target */ }

 private:
  RadioDriver* radio_ = nullptr;
};

}  // namespace corefw
