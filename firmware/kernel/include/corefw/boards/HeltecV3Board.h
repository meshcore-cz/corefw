// HeltecV3Board — board package for the Heltec WiFi LoRa 32 V3.
//
// ESP32-S3 + SX1262, SSD1306 OLED. Pin assignments arrive as -D defines emitted
// by corefw from components/boards/heltec-v3/component.yaml, so the wiring lives
// in declarative config and this class assembles the drivers. Unlike the Wio,
// this board has no external QSPI flash — storage is a single internal LittleFS.
#pragma once

#include <corefw/Board.h>

#if defined(COREFW_TARGET)
#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>  // esp_restart()
#include <drivers/radio/sx1262/SX1262Driver.h>

// Battery sense wiring (matches MeshCore's HeltecV3Board; overridable per board).
#ifndef PIN_VBAT_READ
#define PIN_VBAT_READ 1
#endif
#ifndef PIN_ADC_CTRL
#define PIN_ADC_CTRL 37
#endif
#ifndef ADC_MULTIPLIER
#define ADC_MULTIPLIER 5.42f
#endif
#endif

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

  void begin() override {
#if defined(COREFW_TARGET)
    // VEXT gates power to the OLED and other peripherals; it is active-low on the
    // Heltec V3 (drive LOW to enable the 3V3 peripheral rail).
#if defined(PIN_VEXT_EN)
    pinMode(PIN_VEXT_EN, OUTPUT);
    digitalWrite(PIN_VEXT_EN, LOW);
    delay(50);  // let the rail settle before the OLED is probed
#endif
#if defined(PIN_BOARD_SDA) && defined(PIN_BOARD_SCL)
    Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL);
#else
    Wire.begin();
#endif
#if defined(PIN_USER_BTN)
    pinMode(PIN_USER_BTN, INPUT_PULLUP);
#endif
#if defined(P_LORA_TX_LED)
    pinMode(P_LORA_TX_LED, OUTPUT);
    digitalWrite(P_LORA_TX_LED, LOW);
#endif
#endif
  }

  // Battery via the divider on PIN_VBAT_READ, gated by PIN_ADC_CTRL (active low).
  // Same averaging + formula as MeshCore so the reported mV / app % match.
  uint16_t batteryMilliVolts() override {
#if defined(COREFW_TARGET)
    pinMode(PIN_ADC_CTRL, OUTPUT);
    digitalWrite(PIN_ADC_CTRL, LOW);  // enable the divider
    analogReadResolution(10);
    delay(10);
    uint32_t raw = 0;
    for (int i = 0; i < 8; i++) raw += analogRead(PIN_VBAT_READ);
    raw /= 8;
    digitalWrite(PIN_ADC_CTRL, HIGH);  // disable to save power
    return uint16_t(ADC_MULTIPLIER * (3.3f / 1024.0f) * float(raw) * 1000.0f);
#else
    return 0;
#endif
  }

  RadioDriver* radio() override {
#if defined(COREFW_TARGET)
    return &radio_;
#else
    return nullptr;
#endif
  }
  void reboot() override {
#if defined(COREFW_TARGET)
    esp_restart();
#endif
  }

 private:
#if defined(COREFW_TARGET)
  SX1262Driver radio_;
#endif
};

}  // namespace corefw
