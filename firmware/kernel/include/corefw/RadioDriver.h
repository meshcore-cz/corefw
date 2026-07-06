// The Radio Driver API — the hardware mechanism the kernel schedules onto.
//
// A radio driver implements transmit/receive and RF configuration for a
// specific transceiver (SX1262, LR1121, ...). It is a *mechanism*: it does not
// decide when to transmit, respect duty cycle, or manage retries — the kernel's
// radio scheduler owns that policy and is the only caller. Modules never see a
// RadioDriver; they use MeshService instead.
#pragma once

#include <cstddef>
#include <cstdint>

namespace corefw {

// LoRa PHY parameters. Defaults mirror the reference firmware's regional config
// so an unconfigured driver is already on a compatible channel.
struct RadioConfig {
  float frequency_mhz = 869.525f;
  float bandwidth_khz = 250.0f;
  uint8_t spreading_factor = 11;
  uint8_t coding_rate = 5;
  int8_t tx_power_dbm = 22;
  uint16_t preamble_len = 8;
  uint8_t sync_word = 0x12;
};

class RadioDriver {
 public:
  virtual ~RadioDriver() = default;

  // Bring the transceiver up with the given parameters. Returns false on
  // hardware init failure.
  virtual bool begin(const RadioConfig& cfg) = 0;

  // Apply new PHY parameters (e.g. after a policy changes tx power).
  virtual bool configure(const RadioConfig& cfg) = 0;

  // Transmit a raw frame. Blocking until the packet is on air or timed out.
  // Only the kernel scheduler calls this.
  virtual bool transmit(const uint8_t* data, size_t len) = 0;

  // Enter continuous receive; received frames are delivered via the callback
  // set with onReceive().
  virtual void startReceive() = 0;

  // Copy the most recently received frame into `buf` (capacity `cap`); returns
  // the frame length, or 0 if none pending.
  virtual size_t readReceived(uint8_t* buf, size_t cap) = 0;

  // Signal quality of the last received frame.
  virtual float lastSNR() const { return 0.0f; }
  virtual int lastRSSI() const { return 0; }

  // Put the radio into its lowest-power state (kernel/power-coordinator driven).
  virtual void sleep() {}
};

}  // namespace corefw
