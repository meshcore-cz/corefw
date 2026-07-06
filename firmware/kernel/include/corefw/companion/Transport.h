// CompanionTransport — the byte pipe to the phone/app (BLE, USB serial or TCP).
//
// The companion module frames Companion Protocol messages over whichever
// transport a profile selects. Keeping this an interface lets the module and its
// framing/command logic stay portable and host-testable, with concrete BLE/USB
// implementations provided per board (target-only).
#pragma once

#include <cstddef>
#include <cstdint>

namespace corefw::companion {

class CompanionTransport {
 public:
  virtual ~CompanionTransport() = default;

  // True while an app is connected.
  virtual bool connected() const = 0;

  // Write raw bytes (a complete framed message) to the app.
  virtual void write(const uint8_t* data, size_t len) = 0;

  // Read up to cap bytes that have arrived from the app; returns the count.
  virtual size_t read(uint8_t* buf, size_t cap) = 0;
};

}  // namespace corefw::companion
