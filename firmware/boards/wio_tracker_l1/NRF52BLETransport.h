// NRF52BLETransport — CompanionTransport over BLE UART for the nRF52840.
//
// TARGET-ONLY: uses the Adafruit Bluefruit stack (as the reference Wio Tracker
// L1 companion does). Presents the app-facing byte pipe the CompanionModule
// frames Companion Protocol messages over. A PIN can be set for secure pairing.
#pragma once

#include <corefw/companion/Transport.h>

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM)

#include <bluefruit.h>

namespace corefw::board {

class NRF52BLETransport : public companion::CompanionTransport {
 public:
  // begin advertises as `name` and starts the Nordic UART service. When
  // pin != 0, LESC passkey pairing is required.
  void begin(const char* name, uint32_t pin = 0) {
    Bluefruit.begin();
    Bluefruit.setName(name);
    if (pin != 0) {
      Bluefruit.Security.setPIN(formatPin(pin));
      Bluefruit.Security.setIOCaps(true, false, false);
    }
    bleuart_.begin();
    startAdvertising();
  }

  bool connected() const override { return Bluefruit.connected() > 0; }

  void write(const uint8_t* data, size_t len) override {
    bleuart_.write(data, len);
  }

  size_t read(uint8_t* buf, size_t cap) override {
    size_t n = 0;
    while (n < cap && bleuart_.available()) {
      buf[n++] = uint8_t(bleuart_.read());
    }
    return n;
  }

 private:
  void startAdvertising() {
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(bleuart_);
    Bluefruit.ScanResponse.addName();
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 244);
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0);
  }

  static const char* formatPin(uint32_t pin) {
    static char buf[7];
    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)(pin % 1000000));
    return buf;
  }

  BLEUart bleuart_;
};

}  // namespace corefw::board

#endif  // COREFW_TARGET && NRF52_PLATFORM
