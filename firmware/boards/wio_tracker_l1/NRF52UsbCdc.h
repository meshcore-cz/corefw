// NRF52UsbCdc — non-blocking USB CDC writes for the Wio Tracker L1 companion.
//
// Adafruit's Serial.write() loops with yield() until the host drains the TX
// FIFO, which freezes the main loop (OLED/buttons) when an advert triggers
// LOG_RX + NEW_ADVERT back-to-back. Use these helpers instead.
#pragma once

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM)

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

#include <cstddef>
#include <cstdint>

namespace corefw::board {

inline bool usbCdcHostOpen() { return Serial.dtr() != 0; }

inline size_t usbCdcWritePartial(const uint8_t* data, size_t len) {
  if (len == 0 || !tud_cdc_connected()) return 0;
  size_t avail = tud_cdc_n_write_available(0);
  if (avail == 0) return 0;
  size_t n = len < avail ? len : avail;
  return tud_cdc_n_write(0, data, n) == n ? n : 0;
}

inline bool usbCdcWrite(const uint8_t* data, size_t len) {
  return usbCdcWritePartial(data, len) == len;
}

}  // namespace corefw::board

#endif  // COREFW_TARGET && NRF52_PLATFORM
