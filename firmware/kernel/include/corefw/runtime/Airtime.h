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
// Airtime budget — MeshCore's token-bucket duty-cycle model (Dispatcher.cpp).
//
// A bucket holding up to a 1-hour window's worth of transmit-time refills at the
// duty fraction of real time; each transmission spends its airtime from it, and
// a send is held only when the bucket falls below a small reserve. So ordinary
// interactive traffic (messages, traces, the occasional advert) never blocks —
// the earlier per-packet "off-time" lockout wrongly stalled every send for ~100x
// its airtime — while sustained flooding is still throttled to the duty fraction.
// The reference default is ~50% (a loose sanity cap, not strict ETSI); a stricter
// region/board can lower it.
class DutyCycleLimiter {
 public:
  explicit DutyCycleLimiter(float duty_fraction = 0.5f) { setDuty(duty_fraction); }

  // Set the sustained duty fraction (0..1). Resets the bucket to full.
  void setDuty(float fraction) {
    duty_ = fraction > 1.0f ? 1.0f : (fraction > 0.0f ? fraction : 0.0001f);
    max_budget_ms_ = uint32_t(double(kWindowMs) * duty_);
    budget_ms_ = max_budget_ms_;
  }

  // allows reports whether a transmission may start now (bucket above reserve).
  bool allows(uint32_t now_ms) const { return refilled(now_ms) >= kReserveMs; }

  // record accounts for a transmission of `airtime_ms` completing at `now_ms`,
  // refilling for elapsed time then spending the airtime from the bucket.
  void record(uint32_t now_ms, uint32_t airtime_ms) {
    uint32_t b = refilled(now_ms);
    budget_ms_ = (b > airtime_ms) ? (b - airtime_ms) : 0;
    last_ms_ = now_ms;
  }

  // Current bucket level (ms of airtime) at `now_ms`, for diagnostics/tests.
  uint32_t budgetMs(uint32_t now_ms) const { return refilled(now_ms); }

 private:
  uint32_t refilled(uint32_t now_ms) const {
    const uint64_t add = uint64_t(double(uint32_t(now_ms - last_ms_)) * duty_);
    const uint64_t b = uint64_t(budget_ms_) + add;
    return b > max_budget_ms_ ? max_budget_ms_ : uint32_t(b);
  }

  static constexpr uint32_t kWindowMs = 3600000;  // 1-hour budget window
  static constexpr uint32_t kReserveMs = 100;     // min bucket level before a TX
  float duty_ = 0.5f;
  uint32_t max_budget_ms_ = 1800000;
  uint32_t budget_ms_ = 1800000;
  uint32_t last_ms_ = 0;
};

}  // namespace corefw
