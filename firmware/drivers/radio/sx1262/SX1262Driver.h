// SX1262Driver — RadioDriver implementation for the Semtech SX1262.
//
// TARGET-ONLY: this file depends on RadioLib and the Arduino SPI stack, so it is
// compiled for device builds, not the host test suite. It implements the kernel
// RadioDriver mechanism; the kernel scheduler is the only caller. Pin wiring
// arrives as -D defines from the active board package (P_LORA_NSS, P_LORA_BUSY,
// P_LORA_DIO_1, P_LORA_RESET, SX126X_DIO3_TCXO_VOLTAGE, ...), keeping this
// driver board-agnostic.
#pragma once

#include <corefw/RadioDriver.h>

#if defined(COREFW_TARGET)  // set by the generated PlatformIO env

#include <SPI.h>
#include <RadioLib.h>

namespace corefw {

// MeshCore's RadioLibWrapper arms DIO1 for RX/TX done and polls a flag in
// recvRaw(); polling the pin directly can miss packets between loop() calls.
class SX1262Driver : public RadioDriver {
 public:
  SX1262Driver()
      : radio_(new ::Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI)) {}

  bool begin(const RadioConfig& cfg) override {
#if defined(P_LORA_SCLK)
#if defined(NRF52_PLATFORM)
    SPI.setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI);
    SPI.begin();
#else
    SPI.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
#endif
#endif
    pinMode(P_LORA_DIO_1, INPUT);
#ifdef SX126X_USE_REGULATOR_LDO
    constexpr bool useRegulatorLDO = SX126X_USE_REGULATOR_LDO;
#else
    constexpr bool useRegulatorLDO = false;
#endif
    int16_t st = radio_.begin(cfg.frequency_mhz, cfg.bandwidth_khz, cfg.spreading_factor,
                              cfg.coding_rate, cfg.sync_word, cfg.tx_power_dbm, cfg.preamble_len,
                              SX126X_DIO3_TCXO_VOLTAGE, useRegulatorLDO);
    if (st == RADIOLIB_ERR_SPI_CMD_FAILED || st == RADIOLIB_ERR_SPI_CMD_INVALID) {
      st = radio_.begin(cfg.frequency_mhz, cfg.bandwidth_khz, cfg.spreading_factor,
                        cfg.coding_rate, cfg.sync_word, cfg.tx_power_dbm, cfg.preamble_len,
                        0.0f, useRegulatorLDO);
    }
    if (st != RADIOLIB_ERR_NONE) return false;
    radio_.setCRC(true);
#ifdef SX126X_DIO2_AS_RF_SWITCH
    radio_.setDio2AsRfSwitch(SX126X_DIO2_AS_RF_SWITCH);
#endif
#if defined(SX126X_RXEN) || defined(SX126X_TXEN)
#ifndef SX126X_RXEN
#define SX126X_RXEN RADIOLIB_NC
#endif
#ifndef SX126X_TXEN
#define SX126X_TXEN RADIOLIB_NC
#endif
    radio_.setRfSwitchPins(SX126X_RXEN, SX126X_TXEN);
#endif
#ifdef SX126X_RX_BOOSTED_GAIN
    radio_.setRxBoostedGainMode(SX126X_RX_BOOSTED_GAIN);
#endif
    radio_.setCurrentLimit(SX126X_CURRENT_LIMIT);
    radio_.setPacketReceivedAction(onDio1Action);
    started_ = true;
    irq_ready_ = false;
    startReceive();
    return true;
  }

  bool configure(const RadioConfig& cfg) override {
    bool ok = true;
    ok = ok && radio_.setFrequency(cfg.frequency_mhz) == RADIOLIB_ERR_NONE;
    ok = ok && radio_.setBandwidth(cfg.bandwidth_khz) == RADIOLIB_ERR_NONE;
    ok = ok && radio_.setSpreadingFactor(cfg.spreading_factor) == RADIOLIB_ERR_NONE;
    ok = ok && radio_.setCodingRate(cfg.coding_rate) == RADIOLIB_ERR_NONE;
    ok = ok && radio_.setOutputPower(cfg.tx_power_dbm) == RADIOLIB_ERR_NONE;
    startReceive();
    return ok;
  }

  bool transmit(const uint8_t* data, size_t len) override {
    // Blink the TX LED for the duration of the transmit, matching MeshCore's
    // onBeforeTransmit/onAfterTransmit — so the board LED flickers on each packet
    // this node sends or forwards (adverts, messages, traces, flood forwards).
#ifdef P_LORA_TX_LED
    digitalWrite(P_LORA_TX_LED, HIGH);
#endif
    bool ok = radio_.transmit(const_cast<uint8_t*>(data), len) == RADIOLIB_ERR_NONE;
#ifdef P_LORA_TX_LED
    digitalWrite(P_LORA_TX_LED, LOW);
#endif
    irq_ready_ = false;  // TX done shares the same DIO1 action
    startReceive();
    return ok;
  }

  void startReceive() override {
    if (!started_) return;
    if (radio_.startReceive() == RADIOLIB_ERR_NONE) in_rx_ = true;
  }

  size_t readReceived(uint8_t* buf, size_t cap) override {
    if (!irq_ready_) {
      if ((radio_.getIrqFlags() & RADIOLIB_SX126X_IRQ_RX_DONE) == 0) {
        ensureReceive();
        return 0;
      }
    }
    irq_ready_ = false;
    in_rx_ = false;

    size_t n = radio_.getPacketLength();
    if (n == 0 || n > cap) {
      uint8_t discard[1];
      radio_.readData(discard, 0);
      startReceive();
      return 0;
    }
    if (radio_.readData(buf, n) != RADIOLIB_ERR_NONE) {
      startReceive();
      return 0;
    }
    last_snr_ = radio_.getSNR();
    last_rssi_ = int(radio_.getRSSI());
    startReceive();
    return n;
  }

  float lastSNR() const override { return last_snr_; }
  int lastRSSI() const override { return last_rssi_; }
  int16_t noiseFloorDbm() const override { return noise_floor_dbm_; }
  void sleep() override { radio_.sleep(); }

  void loop() override {
    if (!started_ || !in_rx_) return;
    if (isReceivingPacket()) return;
    if (num_floor_samples_ < kNumNoiseFloorSamples) {
      int rssi = int(radio_.getRSSI(false));
      if (rssi < noise_floor_dbm_ + kSamplingThresholdDb) {
        num_floor_samples_++;
        floor_sample_sum_ += rssi;
      }
    } else if (floor_sample_sum_ != 0) {
      noise_floor_dbm_ = int16_t(floor_sample_sum_ / kNumNoiseFloorSamples);
      if (noise_floor_dbm_ < -120) noise_floor_dbm_ = -120;
      floor_sample_sum_ = 0;
      num_floor_samples_ = 0;
    }
  }

 private:
  static constexpr int kNumNoiseFloorSamples = 64;
  static constexpr int kSamplingThresholdDb = 14;

  bool isReceivingPacket() {
    uint16_t irq = radio_.getIrqFlags();
    return (irq & RADIOLIB_SX126X_IRQ_HEADER_VALID) != 0 ||
           (irq & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) != 0;
  }

  static void onDio1Action() { irq_ready_ = true; }

  void ensureReceive() {
    if (!started_ || in_rx_) return;
    if (radio_.startReceive() == RADIOLIB_ERR_NONE) in_rx_ = true;
  }

  SX1262 radio_;
  bool started_ = false;
  bool in_rx_ = false;
  float last_snr_ = 0.0f;
  int last_rssi_ = 0;
  int16_t noise_floor_dbm_ = 0;
  uint16_t num_floor_samples_ = 0;
  int32_t floor_sample_sum_ = 0;
  static volatile bool irq_ready_;
};

inline volatile bool SX1262Driver::irq_ready_ = false;

}  // namespace corefw

#endif  // COREFW_TARGET
