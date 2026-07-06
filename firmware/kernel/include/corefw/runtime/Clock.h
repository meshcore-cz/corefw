// Clock — the kernel's monotonic time source.
//
// Abstracted so the runtime (scheduler, airtime limiter, retransmit delays) can
// be driven by a virtual clock in host tests and by millis()/esp_timer on
// device.
#pragma once

#include <cstdint>

namespace corefw {

class Clock {
 public:
  virtual ~Clock() = default;
  virtual uint32_t millis() const = 0;
};

}  // namespace corefw
