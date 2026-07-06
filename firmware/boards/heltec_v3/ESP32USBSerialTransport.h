// ESP32USBSerialTransport — CompanionTransport over the ESP32-S3 USB CDC.
//
// TARGET-ONLY. The simplest reliable transport: the framed companion protocol
// runs straight over Serial, so a desktop app (or a serial bridge) can talk to
// the Heltec companion with no BLE stack involved.
#pragma once

#include <corefw/companion/Transport.h>

#if defined(COREFW_TARGET)

#include <Arduino.h>

namespace corefw::board {

class ESP32USBSerialTransport : public companion::CompanionTransport {
 public:
  void begin(uint32_t baud = 115200) { Serial.begin(baud); }

  bool connected() const override { return true; }

  bool write(const uint8_t* data, size_t len) override {
    return Serial.write(data, len) == len;
  }

  size_t read(uint8_t* buf, size_t cap) override {
    size_t n = 0;
    while (n < cap && Serial.available() > 0) buf[n++] = uint8_t(Serial.read());
    return n;
  }
};

}  // namespace corefw::board

#endif  // COREFW_TARGET
