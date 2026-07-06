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
#define SH110X_NO_SPLASH
#include <Adafruit_SH110X.h>
#include <Wire.h>

namespace corefw::ui {

#ifndef DISPLAY_ADDRESS
#define DISPLAY_ADDRESS 0x3C
#endif
#ifndef PIN_OLED_RESET
#define PIN_OLED_RESET -1
#endif

class SH1106Display : public Display {
 public:
  SH1106Display(uint8_t i2c_addr = DISPLAY_ADDRESS, int reset = PIN_OLED_RESET)
      : panel_(128, 64, &Wire, reset), addr_(i2c_addr) {}

  bool begin() {
    if (!panel_.begin(addr_, true)) return false;
    panel_.cp437(true);
    turnOn();
    startFrame();
    endFrame();
    return true;
  }

  int width() const override { return 128; }
  int height() const override { return 64; }

  bool isOn() const override { return on_; }
  void turnOn() override {
    panel_.oled_command(SH110X_DISPLAYON);
    on_ = true;
  }
  void turnOff() override {
    panel_.oled_command(SH110X_DISPLAYOFF);
    on_ = false;
  }
  void clear() override {
    panel_.clearDisplay();
    panel_.display();
  }
  void startFrame(Color bkg = Display::DARK) override {
    (void)bkg;
    panel_.clearDisplay();
    color_ = SH110X_WHITE;
    panel_.setTextColor(color_);
    panel_.setTextSize(1);
  }
  void setTextSize(int sz) override { panel_.setTextSize(sz); }
  void setColor(Color c) override {
    color_ = c == DARK ? SH110X_BLACK : SH110X_WHITE;
    panel_.setTextColor(color_);
  }
  void setCursor(int x, int y) override { panel_.setCursor(x, y); }
  void print(const char* text) override { panel_.print(text); }
  void printWordWrap(const char* text, int max_width) override {
    if (!text) return;
    int line_x = panel_.getCursorX();
    int line_y = panel_.getCursorY();
    char word[32];
    size_t wi = 0;
    for (size_t i = 0;; ++i) {
      char c = text[i];
      if (c != 0 && c != ' ' && c != '\n' && wi < sizeof(word) - 1) {
        word[wi++] = c;
        continue;
      }
      if (wi > 0) {
        word[wi] = 0;
        int word_w = getTextWidth(word);
        if (panel_.getCursorX() > line_x && panel_.getCursorX() + word_w > line_x + max_width) {
          line_y += 10;
          panel_.setCursor(line_x, line_y);
        }
        panel_.print(word);
        wi = 0;
      }
      if (c == 0) break;
      if (c == '\n') {
        line_y += 10;
        panel_.setCursor(line_x, line_y);
      } else if (panel_.getCursorX() + 6 <= line_x + max_width) {
        panel_.print(' ');
      }
    }
  }
  void fillRect(int x, int y, int w, int h) override { panel_.fillRect(x, y, w, h, color_); }
  void drawRect(int x, int y, int w, int h) override { panel_.drawRect(x, y, w, h, color_); }
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override {
    panel_.drawBitmap(x, y, bits, w, h, color_);
  }
  uint16_t getTextWidth(const char* text) override {
    if (!text) return 0;
    int16_t x1, y1;
    uint16_t w, h;
    panel_.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    return w;
  }
  void hline(int y) override { panel_.drawFastHLine(0, y, 128, color_); }
  void flush() override { panel_.display(); }
  void endFrame() override { flush(); }

 private:
  Adafruit_SH1106G panel_;
  uint8_t addr_;
  uint16_t color_ = SH110X_WHITE;
  bool on_ = false;
};

}  // namespace corefw::ui

#endif  // COREFW_TARGET
