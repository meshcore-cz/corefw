// WioTrackerL1Board — board package for the Seeed Wio Tracker L1.
//
// nRF52840 + SX1262, SH1106 OLED, on-board GPS. Pin assignments arrive as -D
// defines emitted by corefw from components/boards/wio-tracker-l1/component.yaml.
#pragma once

#include <corefw/Board.h>

#if defined(COREFW_TARGET)
#include <Arduino.h>
#include <Wire.h>
#include <drivers/radio/sx1262/SX1262Driver.h>
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

  // VBAT is read through a 2:1 divider on PIN_VBAT_READ against the 3.6V
  // internal reference — same formula as MeshCore's WioTrackerL1Board so the
  // reported millivolts (and the app's battery %) match the reference firmware.
  // Cached and re-sampled at most every 2s: the ADC settle delay would otherwise
  // stutter the main loop, since the UI reads this on every refresh.
  uint16_t batteryMilliVolts() override {
#if defined(COREFW_TARGET)
    uint32_t now = millis();
    if (batt_mv_ != 0 && (now - batt_sampled_ms_) < kBatterySampleIntervalMs) {
      return batt_mv_;
    }
    analogReadResolution(12);
    analogReference(AR_INTERNAL);
    delay(10);
    int adc = analogRead(PIN_VBAT_READ);
    batt_mv_ = uint16_t((adc * ADC_MULTIPLIER * AREF_VOLTAGE) / 4.096);
    batt_sampled_ms_ = now;
    return batt_mv_;
#else
    return 0;
#endif
  }

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
  RadioDriver* radio() override {
#if defined(COREFW_TARGET)
    return &radio_;
#else
    return nullptr;
#endif
  }
  void reboot() override { /* NVIC_SystemReset() on target */ }

 private:
  static constexpr uint32_t kBatterySampleIntervalMs = 2000;
  uint16_t batt_mv_ = 0;
  uint32_t batt_sampled_ms_ = 0;
#if defined(COREFW_TARGET)
  SX1262Driver radio_;
#endif
};

}  // namespace corefw
