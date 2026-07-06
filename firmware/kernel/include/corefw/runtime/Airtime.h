// Airtime & duty-cycle enforcement.
//
// LoRa is subject to regional duty-cycle limits (e.g. EU 868 sub-bands at 1%).
// The kernel — never a module — is responsible for staying within budget, so
// the whole mesh remains a good radio citizen. This computes time-on-air with
// the Semtech formula and enforces the per-sub-band off-time rule
// (t_off = t_on * (1/duty - 1)).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace corefw {

// timeOnAirMs returns the LoRa time-on-air, in milliseconds, for a `payload_len`
// byte frame at the given PHY parameters. Assumes explicit header + CRC on, as
// MeshCore uses.
inline uint32_t timeOnAirMs(size_t payload_len, float bandwidth_khz, uint8_t sf,
                            uint8_t coding_rate = 5, uint16_t preamble_len = 8) {
  const double bw = double(bandwidth_khz) * 1000.0;
  const double tsym = std::pow(2.0, sf) / bw;  // seconds per symbol
  // Low-data-rate optimisation enabled when symbol time > 16 ms.
  const int de = (tsym > 0.016) ? 1 : 0;
  const int cr = coding_rate - 4;  // 5..8 -> 1..4
  const double preamble = (preamble_len + 4.25) * tsym;
  const double numerator = 8.0 * double(payload_len) - 4.0 * sf + 28.0 + 16.0 /*CRC*/ - 20.0 * 0 /*IH*/;
  const double denom = 4.0 * (sf - 2 * de);
  double n = std::ceil(numerator / denom) * (cr + 4);
  if (n < 0) n = 0;
  const double payload_syms = 8.0 + n;
  const double toa = preamble + payload_syms * tsym;
  return uint32_t(toa * 1000.0 + 0.5);
}

// DutyCycleLimiter enforces a single sub-band's duty cycle across time.
class DutyCycleLimiter {
 public:
  explicit DutyCycleLimiter(float duty_fraction = 0.01f) : duty_(duty_fraction) {}

  void setDuty(float fraction) { duty_ = fraction; }

  // allows reports whether a transmission may start now.
  bool allows(uint32_t now_ms) const { return int32_t(now_ms - next_allowed_ms_) >= 0; }

  // record accounts for a transmission of `airtime_ms` starting at `now_ms`,
  // scheduling the earliest next permitted transmission.
  void record(uint32_t now_ms, uint32_t airtime_ms) {
    // Next allowed = end-of-tx + off-time, where off = airtime*(1/duty - 1),
    // i.e. next_allowed = now + airtime/duty.
    const uint32_t gap = uint32_t(double(airtime_ms) / (duty_ > 0 ? duty_ : 1.0));
    next_allowed_ms_ = now_ms + gap;
  }

  uint32_t nextAllowedMs() const { return next_allowed_ms_; }

 private:
  float duty_;
  uint32_t next_allowed_ms_ = 0;
};

}  // namespace corefw
