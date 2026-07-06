// SH1106Display — Display implementation for the 128x64 SH1106 OLED on the Wio
// Tracker L1.
//
// TARGET-ONLY: depends on Adafruit_SH110X + Adafruit_GFX, so it is compiled for
// device builds, not the host test suite. The portable screen logic lives in
// CompanionUI; this class only draws what the UI asks onto the panel.
#pragma once

#include <corefw/ui/Display.h>

#if defined(COREFW_TARGET)

#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Wire.h>

namespace corefw::ui {

class SH1106Display : public Display {
 public:
  SH1106Display(uint8_t i2c_addr = 0x3C, int reset = -1)
      : panel_(128, 64, &Wire, reset), addr_(i2c_addr) {}

  bool begin() {
    if (!panel_.begin(addr_, true)) return false;
    panel_.clearDisplay();
    panel_.setTextSize(1);
    panel_.setTextColor(SH110X_WHITE);
    panel_.display();
    return true;
  }

  int width() const override { return 128; }
  int height() const override { return 64; }

  void clear() override { panel_.clearDisplay(); }
  void setCursor(int x, int y) override { panel_.setCursor(x, y); }
  void print(const char* text) override { panel_.print(text); }
  void hline(int y) override { panel_.drawFastHLine(0, y, 128, SH110X_WHITE); }
  void flush() override { panel_.display(); }

 private:
  Adafruit_SH1106G panel_;
  uint8_t addr_;
};

}  // namespace corefw::ui

#endif  // COREFW_TARGET
