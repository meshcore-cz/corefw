// NRF52USBSerialTransport — CompanionTransport over Arduino USB Serial.
//
// TARGET-ONLY. This matches MeshCore's WioTrackerL1_companion_radio_usb path:
// bytes are carried over the board's USB CDC Serial object, while the portable
// CompanionModule owns the '<'/'>' frame codec.
#pragma once

#include <corefw/companion/Transport.h>

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM)

#include <Arduino.h>

namespace corefw::board {

class NRF52USBSerialTransport : public companion::CompanionTransport {
 public:
  void begin(uint32_t baud = 115200) { Serial.begin(baud); }

  bool connected() const override { return true; }

  void write(const uint8_t* data, size_t len) override { Serial.write(data, len); }

  size_t read(uint8_t* buf, size_t cap) override {
    size_t n = 0;
    while (n < cap && Serial.available()) {
      int c = Serial.read();
      if (c < 0) break;
      buf[n++] = uint8_t(c);
    }
    return n;
  }
};

}  // namespace corefw::board

#endif  // COREFW_TARGET && NRF52_PLATFORM
