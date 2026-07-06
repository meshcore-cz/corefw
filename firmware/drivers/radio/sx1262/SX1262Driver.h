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

#include <RadioLib.h>

namespace corefw {

class SX1262Driver : public RadioDriver {
 public:
  SX1262Driver()
      : radio_(new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY)) {}

  bool begin(const RadioConfig& cfg) override {
    int16_t st = radio_.begin(cfg.frequency_mhz, cfg.bandwidth_khz, cfg.spreading_factor,
                              cfg.coding_rate, cfg.sync_word, cfg.tx_power_dbm, cfg.preamble_len,
                              SX126X_DIO3_TCXO_VOLTAGE);
#ifdef SX126X_DIO2_AS_RF_SWITCH
    radio_.setDio2AsRfSwitch(true);
#endif
#ifdef SX126X_RX_BOOSTED_GAIN
    radio_.setRxBoostedGainMode(true);
#endif
    radio_.setCurrentLimit(SX126X_CURRENT_LIMIT);
    return st == RADIOLIB_ERR_NONE;
  }

  bool configure(const RadioConfig& cfg) override {
    radio_.setFrequency(cfg.frequency_mhz);
    radio_.setBandwidth(cfg.bandwidth_khz);
    radio_.setSpreadingFactor(cfg.spreading_factor);
    radio_.setCodingRate(cfg.coding_rate);
    return radio_.setOutputPower(cfg.tx_power_dbm) == RADIOLIB_ERR_NONE;
  }

  bool transmit(const uint8_t* data, size_t len) override {
    return radio_.transmit(const_cast<uint8_t*>(data), len) == RADIOLIB_ERR_NONE;
  }

  void startReceive() override { radio_.startReceive(); }

  size_t readReceived(uint8_t* buf, size_t cap) override {
    size_t n = radio_.getPacketLength();
    if (n == 0 || n > cap) return 0;
    if (radio_.readData(buf, n) != RADIOLIB_ERR_NONE) return 0;
    last_snr_ = radio_.getSNR();
    last_rssi_ = int(radio_.getRSSI());
    return n;
  }

  float lastSNR() const override { return last_snr_; }
  int lastRSSI() const override { return last_rssi_; }
  void sleep() override { radio_.sleep(); }

 private:
  SX1262 radio_;
  float last_snr_ = 0.0f;
  int last_rssi_ = 0;
};

}  // namespace corefw

#endif  // COREFW_TARGET
