// SenseCapSolarBoard — board package for the Seeed SenseCAP Solar Node P1.
//
// nRF52840 (XIAO nRF52840 module) + SX1262 with an external RX/TX switch
// (SX126X_RXEN), an L76KB GPS on Serial1, and a solar/LiPo battery — no display
// (headless outdoor repeater). Pin assignments arrive as -D defines emitted by
// corefw from components/boards/sensecap-solar/component.yaml, plus the Arduino
// pin aliases (BATTERY_PIN, VBAT_ENABLE, ADC_MULTIPLIER, AREF_VOLTAGE) from the
// board's variant.h. The shared nRF52 target main (firmware/platform/nrf52) does
// all the runtime wiring; this class only exposes the board's peripherals.
#pragma once

#include <corefw/Board.h>

#if defined(COREFW_TARGET)
#include <Arduino.h>
#include <Wire.h>
#include <drivers/radio/sx1262/SX1262Driver.h>
#endif

namespace corefw {

class SenseCapSolarBoard : public Board {
 public:
  const char* manufacturerName() const override { return "Seeed Studio"; }
  const char* boardName() const override { return "SenseCAP Solar"; }

  BoardCapabilities capabilities() const override {
    BoardCapabilities c;
    c.radio = c.battery = c.gps = true;
    c.bluetooth = c.usb = c.deep_sleep = true;
    c.display = false;  // headless solar node
    c.wifi = false;
    return c;
  }

  int maxTxPowerDbm() const override { return 22; }

  // Battery is read through a 1M/512k divider on BATTERY_PIN against the 3.0V
  // internal reference; VBAT_ENABLE (active-low) gates the divider. Same formula
  // as MeshCore's SenseCapSolarBoard so the reported millivolts (and the app's
  // battery %) match the reference firmware. Cached/2s so the ADC settle delay
  // doesn't stutter the main loop.
  uint16_t batteryMilliVolts() override {
#if defined(COREFW_TARGET)
    uint32_t now = millis();
    if (batt_mv_ != 0 && (now - batt_sampled_ms_) < kBatterySampleIntervalMs) {
      return batt_mv_;
    }
    digitalWrite(VBAT_ENABLE, LOW);
    analogReadResolution(12);
    analogReference(AR_INTERNAL_3_0);
    delay(10);
    int adc = analogRead(BATTERY_PIN);
    batt_mv_ = uint16_t((adc * ADC_MULTIPLIER * AREF_VOLTAGE) / 4.096);
    batt_sampled_ms_ = now;
    return batt_mv_;
#else
    return 0;
#endif
  }

  void begin() override {
#if defined(COREFW_TARGET)
    pinMode(BATTERY_PIN, INPUT);
    pinMode(VBAT_ENABLE, OUTPUT);
    digitalWrite(VBAT_ENABLE, LOW);  // enable the battery divider
    analogReadResolution(12);
    analogReference(AR_INTERNAL_3_0);
#if defined(PIN_USER_BTN)
    pinMode(PIN_USER_BTN, INPUT_PULLUP);
#endif
#if defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
    Wire.setPins(PIN_WIRE_SDA, PIN_WIRE_SCL);
#endif
    Wire.begin();
#if defined(P_LORA_TX_LED)
    pinMode(P_LORA_TX_LED, OUTPUT);
    digitalWrite(P_LORA_TX_LED, LOW);
#endif
    delay(10);  // give the sx1262 time to power up
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
