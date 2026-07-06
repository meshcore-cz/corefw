// ArduinoBuzzer — ToneOutput backed by the Arduino tone()/noTone() API driving
// the Wio Tracker L1 piezo buzzer (PIN_BUZZER = 12).
//
// TARGET-ONLY. The melody parsing and non-blocking sequencing live in the
// portable ui::Melody; this only produces the actual square wave on the pin.
#pragma once

#include <corefw/ui/Buzzer.h>

#if defined(COREFW_TARGET)

#include <Arduino.h>

namespace corefw::ui {

class ArduinoBuzzer : public ToneOutput {
 public:
  explicit ArduinoBuzzer(int pin) : pin_(pin) {}

  void begin() { pinMode(pin_, OUTPUT); }

  void tone(uint16_t freq) override {
    if (freq > 0) ::tone(pin_, freq);
    else ::noTone(pin_);
  }
  void noTone() override { ::noTone(pin_); }

 private:
  int pin_;
};

}  // namespace corefw::ui

#endif  // COREFW_TARGET
