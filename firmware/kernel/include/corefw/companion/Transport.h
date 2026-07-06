// CompanionTransport — the byte pipe to the phone/app (BLE, USB serial or TCP).
//
// The companion module frames Companion Protocol messages over whichever
// transport a profile selects. Keeping this an interface lets the module and its
// framing/command logic stay portable and host-testable, with concrete BLE/USB
// implementations provided per board (target-only).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace corefw::companion {

// Format the BLE GAP name as "MeshCore-<node_name>" for app compatibility.
// Truncate to 31 bytes — the nRF52 SoftDevice and ESP32 NimBLE stacks reject
// longer names.
inline void formatBleDeviceName(char* out, size_t out_cap, const char* node_name) {
  if (out_cap == 0) return;
  std::snprintf(out, out_cap, "MeshCore-%s", node_name ? node_name : "");
  out[out_cap - 1] = '\0';
  if (std::strlen(out) > 31) out[31] = '\0';
}

class CompanionTransport {
 public:
  virtual ~CompanionTransport() = default;

  // True while an app is connected.
  virtual bool connected() const = 0;

  // Write raw bytes (a complete framed message) to the app. Returns false if the
  // transport could not accept the full frame without blocking (USB CDC).
  virtual bool write(const uint8_t* data, size_t len) = 0;

  // Write as much as fits without blocking. Default: all-or-nothing via write().
  virtual size_t writePartial(const uint8_t* data, size_t len) {
    return write(data, len) ? len : 0;
  }

  // Drain any outbound queue (BLE). Called from the companion loop.
  virtual void poll() {}

  // Read up to cap bytes that have arrived from the app; returns the count.
  virtual size_t read(uint8_t* buf, size_t cap) = 0;
};

}  // namespace corefw::companion
