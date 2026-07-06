// NRF52Gps — Wio Tracker L1 L76KB GNSS on Serial1, feeding the portable
// NmeaParser. TARGET-ONLY: the parsing/position logic is host-tested in
// drivers/gps/NmeaParser.h; this only drives the UART and enable pin.
#pragma once

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM)

#include <Arduino.h>

#include <drivers/gps/NmeaParser.h>

namespace corefw::board {

class NRF52Gps {
 public:
  // Power the module and open the UART. The L76KB streams NMEA at 9600 baud on
  // PIN_GPS_TX/RX (wired to Serial1 in the variant).
  void begin() {
#if defined(PIN_GPS_EN)
    pinMode(PIN_GPS_EN, OUTPUT);
    digitalWrite(PIN_GPS_EN, HIGH);  // release standby / enable the regulator
#endif
    // Serial1 is bound to PIN_SERIAL1_RX/TX by the board variant (same wiring
    // MeshCore uses); just open it at the receiver's baud rate.
    Serial1.begin(GPS_BAUDRATE);
    started_ = true;
  }

  // Cut power to the GNSS (used when the app disables GPS).
  void stop() {
#if defined(PIN_GPS_EN)
    if (started_) digitalWrite(PIN_GPS_EN, LOW);
#endif
    started_ = false;
  }

  bool started() const { return started_; }

  // Drain whatever the receiver has sent since the last call. Cheap and
  // non-blocking; call it from the main loop.
  void loop() {
    if (!started_) return;
    while (Serial1.available() > 0) {
      parser_.feed(char(Serial1.read()));
    }
  }

  bool hasFix() const { return parser_.hasFix(); }
  int32_t latE6() const { return parser_.latE6(); }
  int32_t lonE6() const { return parser_.lonE6(); }
  uint32_t unixTime() const { return parser_.unixTime(); }
  uint8_t satellites() const { return parser_.satellites(); }

 private:
  gps::NmeaParser parser_;
  bool started_ = false;
};

}  // namespace corefw::board

#endif  // COREFW_TARGET && NRF52_PLATFORM
