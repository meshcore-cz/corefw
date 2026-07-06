// SSD1306Display — Display implementation for the 128x64 SSD1306 OLED on the
// Heltec WiFi LoRa 32 V3 (0.96" I2C panel).
//
// TARGET-ONLY: depends on Adafruit_SSD1306 + Adafruit_GFX, so it is compiled for
// device builds, not the host test suite. The portable screen logic lives in
// CompanionUI; this class only draws what the UI asks onto the panel. It mirrors
// the Wio's SH1106Display so the same UI renders identically on either board.
#pragma once

#include <corefw/ui/Display.h>

#if defined(COREFW_TARGET)

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

namespace corefw::ui {

#ifndef DISPLAY_ADDRESS
#define DISPLAY_ADDRESS 0x3C
#endif
#ifndef PIN_OLED_RESET
#define PIN_OLED_RESET -1
#endif

class SSD1306Display : public Display {
 public:
  SSD1306Display(uint8_t i2c_addr = DISPLAY_ADDRESS, int reset = PIN_OLED_RESET)
      : panel_(128, 64, &Wire, reset), addr_(i2c_addr) {}

  bool begin() {
    // reset=true pulses PIN_OLED_RESET (GPIO21 on Heltec V3) — required or the
    // panel never initialises. periphBegin=false: the board already did
    // Wire.begin(SDA,SCL) on the correct pins; letting Adafruit call Wire.begin()
    // with no args would re-init I2C on the ESP32 default pins (which collide
    // with the LoRa SPI bus) and orphan the OLED.
    if (!panel_.begin(SSD1306_SWITCHCAPVCC, addr_, /*reset=*/true, /*periphBegin=*/false))
      return false;
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
    panel_.ssd1306_command(SSD1306_DISPLAYON);
    on_ = true;
  }
  void turnOff() override {
    panel_.ssd1306_command(SSD1306_DISPLAYOFF);
    on_ = false;
  }
  void clear() override {
    panel_.clearDisplay();
    panel_.display();
  }
  void startFrame(Color bkg = Display::DARK) override {
    (void)bkg;
    panel_.clearDisplay();
    color_ = SSD1306_WHITE;
    panel_.setTextColor(color_);
    panel_.setTextSize(1);
  }
  void setTextSize(int sz) override { panel_.setTextSize(sz); }
  void setColor(Color c) override {
    color_ = c == DARK ? SSD1306_BLACK : SSD1306_WHITE;
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
  Adafruit_SSD1306 panel_;
  uint8_t addr_;
  uint16_t color_ = SSD1306_WHITE;
  bool on_ = false;
};

}  // namespace corefw::ui

#endif  // COREFW_TARGET
