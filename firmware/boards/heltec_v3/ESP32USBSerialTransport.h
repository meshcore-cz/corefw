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

  // Non-blocking: write only what fits in the UART TX FIFO right now. The plain
  // Serial.write() blocks (spins) once the FIFO fills until the host drains it,
  // so a burst — e.g. an incoming channel message queuing an offline-queue frame
  // plus a MSG_WAITING tickle — would stall the whole main loop (UI + buttons
  // freeze). The CompanionModule's scheduleIo ring re-drains the remainder on the
  // next tick. Mirrors the Wio's NRF52USBSerialTransport fix.
  size_t writePartial(const uint8_t* data, size_t len) override {
    int avail = Serial.availableForWrite();
    if (avail <= 0) return 0;
    size_t n = len < size_t(avail) ? len : size_t(avail);
    return Serial.write(data, n);  // n <= availableForWrite => does not block
  }

  size_t read(uint8_t* buf, size_t cap) override {
    size_t n = 0;
    while (n < cap && Serial.available() > 0) buf[n++] = uint8_t(Serial.read());
    return n;
  }
};

}  // namespace corefw::board

#endif  // COREFW_TARGET
