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
  static constexpr int kSendQueueDepth = 8;

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

  bool write(const uint8_t* data, size_t len) override {
    if (len > companion::MAX_FRAME_SIZE) return false;
    if (!connected() || len == 0) return false;
    if (send_count_ >= kSendQueueDepth) return false;
    std::memcpy(send_queue_[send_tail_], data, len);
    send_len_[send_tail_] = len;
    send_tail_ = (send_tail_ + 1) % kSendQueueDepth;
    send_count_++;
    return true;
  }

  void poll() override {
    if (send_count_ == 0 || !connected()) return;
    size_t len = send_len_[send_head_];
    size_t written = bleuart_.write(send_queue_[send_head_], len);
    if (written == len) {
      send_head_ = (send_head_ + 1) % kSendQueueDepth;
      send_count_--;
    } else if (written > 0) {
      // Drop corrupted partial frame, matching MeshCore SerialBLEInterface.
      send_head_ = (send_head_ + 1) % kSendQueueDepth;
      send_count_--;
    }
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
  uint8_t send_queue_[kSendQueueDepth][companion::MAX_FRAME_SIZE + 3] = {};
  size_t send_len_[kSendQueueDepth] = {};
  int send_head_ = 0;
  int send_tail_ = 0;
  int send_count_ = 0;
};

}  // namespace corefw::board

#endif  // COREFW_TARGET && NRF52_PLATFORM
