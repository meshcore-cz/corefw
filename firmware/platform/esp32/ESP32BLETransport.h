// ESP32BLETransport — CompanionTransport over BLE using the Nordic UART Service
// (NUS), the same GATT profile the Wio's Bluefruit BLEUart exposes, so the
// MeshCore phone app connects to the Heltec companion identically.
//
// TARGET-ONLY. Uses NimBLE-Arduino. Bytes arrive on the NimBLE host task and are
// consumed on the main loop, so RX/TX go through small SPSC ring buffers guarded
// by portMUX critical sections (ESP32 is dual-core). Outgoing frames are chunked
// to the negotiated MTU because a single BLE notification can't exceed MTU-3.
#pragma once

#include <corefw/companion/Transport.h>

#if defined(COREFW_TARGET)

#include <NimBLEDevice.h>

#include <cstring>
#include <string>

namespace corefw::board {

// Nordic UART Service UUIDs.
#define COREFW_NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define COREFW_NUS_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // app -> device (write)
#define COREFW_NUS_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // device -> app (notify)

class ESP32BLETransport : public companion::CompanionTransport {
 public:
  static constexpr int kFrameQueueDepth = 8;

  void begin(const char* name, uint32_t pin = 0) {
    pin_ = pin;
    NimBLEDevice::init(name);
    NimBLEDevice::setMTU(247);  // request a large MTU; we still honour the result
    if (pin_ != 0) {
      NimBLEDevice::setSecurityAuth(true, true, true);  // bond, MITM, secure connections
      NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
      NimBLEDevice::setSecurityPasskey(pin_ % 1000000);
      NimBLEDevice::setSecurityCallbacks(new SecurityCB(this));
    }

    server_ = NimBLEDevice::createServer();
    server_->setCallbacks(new ServerCB(this));

    NimBLEService* svc = server_->createService(COREFW_NUS_SERVICE);
    uint32_t tx_props = NIMBLE_PROPERTY::NOTIFY;
    uint32_t rx_props = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR;
    if (pin_ != 0) {
      tx_props |= NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN;
      rx_props |= NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN;
    }
    tx_ = svc->createCharacteristic(COREFW_NUS_TX, tx_props);
    NimBLECharacteristic* rx = svc->createCharacteristic(
        COREFW_NUS_RX, rx_props);
    rx->setCallbacks(new RxCB(this));
    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(COREFW_NUS_SERVICE);
    adv->setScanResponse(true);
    adv->start();
  }

  bool connected() const override { return pin_ == 0 ? gap_connected_ : secured_; }

  bool write(const uint8_t* data, size_t len) override {
    return writePartial(data, len) == len;
  }

  // Buffer as much as fits; the frame is drained to BLE in poll().
  size_t writePartial(const uint8_t* data, size_t len) override {
    if (!connected() || len == 0 || len > companion::MAX_FRAME_SIZE) return 0;
    bool accepted = false;
    portENTER_CRITICAL(&tx_mux_);
    if (tx_count_ < kFrameQueueDepth) {
      std::memcpy(tx_queue_[tx_tail_], data, len);
      tx_len_[tx_tail_] = len;
      tx_tail_ = (tx_tail_ + 1) % kFrameQueueDepth;
      tx_count_++;
      accepted = true;
    }
    portEXIT_CRITICAL(&tx_mux_);
    return accepted ? len : 0;
  }

  // Send one complete companion frame per notification. MeshCore's BLE
  // transport is frame-native: the serial '<'/'>' byte-stream wrapper is not
  // used over BLE.
  void poll() override {
    if (!connected() || tx_ == nullptr) return;
    uint8_t frame[companion::MAX_FRAME_SIZE];
    size_t len = 0;
    portENTER_CRITICAL(&tx_mux_);
    if (tx_count_ > 0) {
      len = tx_len_[tx_head_];
      std::memcpy(frame, tx_queue_[tx_head_], len);
      tx_head_ = (tx_head_ + 1) % kFrameQueueDepth;
      tx_count_--;
    }
    portEXIT_CRITICAL(&tx_mux_);
    if (len == 0) return;
    tx_->setValue(frame, len);
    tx_->notify();
  }

  size_t read(uint8_t* buf, size_t cap) override {
    size_t len = 0;
    portENTER_CRITICAL(&rx_mux_);
    if (rx_count_ > 0) {
      len = rx_len_[rx_head_];
      if (len > cap) len = cap;
      std::memcpy(buf, rx_queue_[rx_head_], len);
      rx_head_ = (rx_head_ + 1) % kFrameQueueDepth;
      rx_count_--;
    }
    portEXIT_CRITICAL(&rx_mux_);
    return len;
  }

 private:
  void pushRx(const uint8_t* d, size_t len) {
    if (len == 0 || len > companion::MAX_FRAME_SIZE) return;
    portENTER_CRITICAL(&rx_mux_);
    if (rx_count_ < kFrameQueueDepth) {
      std::memcpy(rx_queue_[rx_tail_], d, len);
      rx_len_[rx_tail_] = len;
      rx_tail_ = (rx_tail_ + 1) % kFrameQueueDepth;
      rx_count_++;
    }
    portEXIT_CRITICAL(&rx_mux_);
  }

  class ServerCB : public NimBLEServerCallbacks {
   public:
    explicit ServerCB(ESP32BLETransport* t) : t_(t) {}
    void onConnect(NimBLEServer*) override {
      t_->gap_connected_ = true;
      t_->secured_ = (t_->pin_ == 0);
    }
    void onDisconnect(NimBLEServer*) override {
      t_->gap_connected_ = false;
      t_->secured_ = false;
      t_->mtu_ = 23;
      t_->clearBuffers();
      NimBLEDevice::startAdvertising();  // allow the next app to reconnect
    }
    void onMTUChange(uint16_t mtu, ble_gap_conn_desc*) override { t_->mtu_ = mtu; }

   private:
    ESP32BLETransport* t_;
  };

  class RxCB : public NimBLECharacteristicCallbacks {
   public:
    explicit RxCB(ESP32BLETransport* t) : t_(t) {}
    void onWrite(NimBLECharacteristic* c) override {
      std::string v = c->getValue();
      t_->pushRx(reinterpret_cast<const uint8_t*>(v.data()), v.size());
    }

   private:
    ESP32BLETransport* t_;
  };

  class SecurityCB : public NimBLESecurityCallbacks {
   public:
    explicit SecurityCB(ESP32BLETransport* t) : t_(t) {}
    uint32_t onPassKeyRequest() override { return t_->pin_ % 1000000; }
    void onPassKeyNotify(uint32_t) override {}
    bool onConfirmPIN(uint32_t) override { return true; }
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
      t_->secured_ = (desc != nullptr && desc->sec_state.encrypted && desc->sec_state.authenticated);
      if (!t_->secured_ && t_->server_ != nullptr) {
        t_->server_->disconnect(desc ? desc->conn_handle : BLE_HS_CONN_HANDLE_NONE);
      }
    }

   private:
    ESP32BLETransport* t_;
  };

  void clearBuffers() {
    portENTER_CRITICAL(&rx_mux_);
    rx_head_ = rx_tail_ = rx_count_ = 0;
    portEXIT_CRITICAL(&rx_mux_);
    portENTER_CRITICAL(&tx_mux_);
    tx_head_ = tx_tail_ = tx_count_ = 0;
    portEXIT_CRITICAL(&tx_mux_);
  }

  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* tx_ = nullptr;
  uint32_t pin_ = 0;
  volatile bool gap_connected_ = false;
  volatile bool secured_ = false;
  volatile uint16_t mtu_ = 23;

  portMUX_TYPE rx_mux_ = portMUX_INITIALIZER_UNLOCKED;
  portMUX_TYPE tx_mux_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t rx_queue_[kFrameQueueDepth][companion::MAX_FRAME_SIZE] = {};
  size_t rx_len_[kFrameQueueDepth] = {};
  volatile int rx_head_ = 0, rx_tail_ = 0, rx_count_ = 0;
  uint8_t tx_queue_[kFrameQueueDepth][companion::MAX_FRAME_SIZE] = {};
  size_t tx_len_[kFrameQueueDepth] = {};
  volatile int tx_head_ = 0, tx_tail_ = 0, tx_count_ = 0;
};

}  // namespace corefw::board

#endif  // COREFW_TARGET
