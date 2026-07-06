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
#include <esp_sleep.h>   // esp_light_sleep_start()
#include <driver/gpio.h>  // gpio wakeup
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
    // Clock the CPU down to the board's rated frequency (80 MHz on the V3 vs the
    // 240 MHz default) — an always-on power saving that stock MeshCore applies in
    // ESP32Board::begin(). The radio/BLE are unaffected. corefw already emits the
    // ESP32_CPU_FREQ define; this is where it takes effect.
#if defined(ESP32_CPU_FREQ)
    setCpuFrequencyMhz(ESP32_CPU_FREQ);
#endif
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
    digitalWrite(P_LORA_TX_LED, LOW);  // active-high: LOW = off (matches MeshCore)
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
  // Light-sleep the ESP32 until a LoRa packet arrives (radio DIO1 goes HIGH) or
  // `max_ms` elapses. Mirrors MeshCore's ESP32Board::sleep. CAUTION: this powers
  // down the RF/BLE controller, so the caller must only invoke it when no BLE or
  // USB companion link is live (the shared main gates this) — otherwise the
  // connection drops. Deep sleep and RAM retention are unaffected.
  void lightSleep(uint32_t max_ms) override {
#if defined(COREFW_TARGET)
    const gpio_num_t wake = static_cast<gpio_num_t>(P_LORA_DIO_1);
    if (gpio_get_level(wake) == 1) return;  // a packet is already waiting; don't sleep
    if (max_ms > 0) esp_sleep_enable_timer_wakeup(uint64_t(max_ms) * 1000ULL);
    esp_sleep_enable_gpio_wakeup();
    gpio_wakeup_enable(wake, GPIO_INTR_HIGH_LEVEL);
    esp_light_sleep_start();
    gpio_wakeup_disable(wake);
    gpio_set_intr_type(wake, GPIO_INTR_POSEDGE);
#else
    (void)max_ms;
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
