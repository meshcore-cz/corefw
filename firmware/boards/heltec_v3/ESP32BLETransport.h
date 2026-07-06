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
  static constexpr size_t kRxRing = 512;
  static constexpr size_t kTxRing = 1024;

  void begin(const char* name, uint32_t pin = 0) {
    (void)pin;
    NimBLEDevice::init(name);
    NimBLEDevice::setMTU(247);  // request a large MTU; we still honour the result

    server_ = NimBLEDevice::createServer();
    server_->setCallbacks(new ServerCB(this));

    NimBLEService* svc = server_->createService(COREFW_NUS_SERVICE);
    tx_ = svc->createCharacteristic(COREFW_NUS_TX, NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* rx = svc->createCharacteristic(
        COREFW_NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCB(this));
    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(COREFW_NUS_SERVICE);
    adv->setScanResponse(true);
    adv->start();
  }

  bool connected() const override { return connected_; }

  bool write(const uint8_t* data, size_t len) override {
    return writePartial(data, len) == len;
  }

  // Buffer as much as fits; the frame is drained to BLE in poll().
  size_t writePartial(const uint8_t* data, size_t len) override {
    if (!connected_) return 0;
    size_t n = 0;
    portENTER_CRITICAL(&tx_mux_);
    while (n < len && tx_count_ < kTxRing) {
      tx_ring_[tx_in_] = data[n++];
      tx_in_ = (tx_in_ + 1) % kTxRing;
      tx_count_++;
    }
    portEXIT_CRITICAL(&tx_mux_);
    return n;
  }

  // Send one notification worth of buffered bytes (capped to the negotiated
  // MTU). Called every companion tick; the fast loop drains large frames.
  void poll() override {
    if (!connected_ || tx_ == nullptr) return;
    size_t cap = mtu_ > 3 ? size_t(mtu_ - 3) : 20;
    if (cap > sizeof(scratch_)) cap = sizeof(scratch_);
    size_t n = 0;
    portENTER_CRITICAL(&tx_mux_);
    while (n < cap && tx_count_ > 0) {
      scratch_[n++] = tx_ring_[tx_out_];
      tx_out_ = (tx_out_ + 1) % kTxRing;
      tx_count_--;
    }
    portEXIT_CRITICAL(&tx_mux_);
    if (n == 0) return;
    tx_->setValue(scratch_, n);
    tx_->notify();
  }

  size_t read(uint8_t* buf, size_t cap) override {
    size_t n = 0;
    portENTER_CRITICAL(&rx_mux_);
    while (n < cap && rx_count_ > 0) {
      buf[n++] = rx_ring_[rx_out_];
      rx_out_ = (rx_out_ + 1) % kRxRing;
      rx_count_--;
    }
    portEXIT_CRITICAL(&rx_mux_);
    return n;
  }

 private:
  void pushRx(const uint8_t* d, size_t len) {
    portENTER_CRITICAL(&rx_mux_);
    for (size_t i = 0; i < len && rx_count_ < kRxRing; i++) {
      rx_ring_[rx_in_] = d[i];
      rx_in_ = (rx_in_ + 1) % kRxRing;
      rx_count_++;
    }
    portEXIT_CRITICAL(&rx_mux_);
  }

  class ServerCB : public NimBLEServerCallbacks {
   public:
    explicit ServerCB(ESP32BLETransport* t) : t_(t) {}
    void onConnect(NimBLEServer*) override { t_->connected_ = true; }
    void onDisconnect(NimBLEServer*) override {
      t_->connected_ = false;
      t_->mtu_ = 23;
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

  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* tx_ = nullptr;
  volatile bool connected_ = false;
  volatile uint16_t mtu_ = 23;

  portMUX_TYPE rx_mux_ = portMUX_INITIALIZER_UNLOCKED;
  portMUX_TYPE tx_mux_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t rx_ring_[kRxRing];
  volatile size_t rx_in_ = 0, rx_out_ = 0, rx_count_ = 0;
  uint8_t tx_ring_[kTxRing];
  volatile size_t tx_in_ = 0, tx_out_ = 0, tx_count_ = 0;
  uint8_t scratch_[244];
};

}  // namespace corefw::board

#endif  // COREFW_TARGET
