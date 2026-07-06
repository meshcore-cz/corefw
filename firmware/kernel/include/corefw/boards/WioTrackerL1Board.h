// WioTrackerL1Board — board package for the Seeed Wio Tracker L1.
//
// nRF52840 + SX1262, SH1106 OLED, on-board GPS. Pin assignments arrive as -D
// defines emitted by corefw from components/boards/wio-tracker-l1/component.yaml.
#pragma once

#include <corefw/Board.h>

#if defined(COREFW_TARGET)
#include <Arduino.h>
#include <Wire.h>
#endif

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

  void begin() override {
#if defined(COREFW_TARGET)
    pinMode(PIN_VBAT_READ, INPUT);
    pinMode(PIN_BUTTON1, INPUT_PULLUP);
    pinMode(PIN_BUTTON2, INPUT_PULLUP);
    pinMode(PIN_BUTTON3, INPUT_PULLUP);
    pinMode(PIN_BUTTON4, INPUT_PULLUP);
    pinMode(PIN_BUTTON5, INPUT_PULLUP);
    pinMode(PIN_BUTTON6, INPUT_PULLUP);
#if defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
    Wire.setPins(PIN_WIRE_SDA, PIN_WIRE_SCL);
#endif
    Wire.begin();
#if defined(P_LORA_TX_LED)
    pinMode(P_LORA_TX_LED, OUTPUT);
    digitalWrite(P_LORA_TX_LED, LOW);
#endif
    delay(10);
#endif
  }
  RadioDriver* radio() override { return radio_; }
  void reboot() override { /* NVIC_SystemReset() on target */ }

 private:
  RadioDriver* radio_ = nullptr;
};

}  // namespace corefw
