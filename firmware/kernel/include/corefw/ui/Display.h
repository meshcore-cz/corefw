// Display abstraction.
//
// A compact text/graphics surface the UI draws onto, so screen logic is
// board-agnostic and host-testable. It intentionally mirrors the small subset
// of MeshCore's DisplayDriver used by the companion "new UI": colors collapse
// to on/off on monochrome panels, but the portable UI can still express intent.
#pragma once

#include <cstdint>
#include <cstring>

namespace corefw::ui {

class Display {
 public:
  enum Color { DARK = 0, LIGHT, RED, GREEN, BLUE, YELLOW, ORANGE };

  virtual ~Display() = default;

  virtual int width() const { return 128; }
  virtual int height() const { return 64; }

  virtual bool isOn() const { return true; }
  virtual void turnOn() {}
  virtual void turnOff() {}

  virtual void clear() = 0;
  virtual void startFrame(Color bkg = DARK) {
    (void)bkg;
    clear();
    setTextSize(1);
    setColor(LIGHT);
  }
  virtual void endFrame() { flush(); }
  virtual void setTextSize(int sz) { (void)sz; }
  virtual void setColor(Color c) { (void)c; }
  virtual void setCursor(int x, int y) = 0;
  virtual void print(const char* text) = 0;
  virtual void printWordWrap(const char* text, int max_width) {
    (void)max_width;
    print(text);
  }
  virtual void fillRect(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
  }
  virtual void drawRect(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
  }
  virtual void drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
    (void)x; (void)y; (void)bits; (void)w; (void)h;
  }
  virtual uint16_t getTextWidth(const char* text) {
    return uint16_t((text ? std::strlen(text) : 0) * 6);
  }
  // Draw a horizontal separator at row y.
  virtual void hline(int y) { drawRect(0, y, width(), 1); }
  virtual void drawTextCentered(int mid_x, int y, const char* text) {
    int w = getTextWidth(text);
    setCursor(mid_x - w / 2, y);
    print(text);
  }
  virtual void drawTextRightAlign(int x_anch, int y, const char* text) {
    int w = getTextWidth(text);
    setCursor(x_anch - w, y);
    print(text);
  }
  virtual void drawTextLeftAlign(int x_anch, int y, const char* text) {
    setCursor(x_anch, y);
    print(text);
  }
  virtual void translateUTF8ToBlocks(char* dest, const char* src, size_t dest_size) {
    if (dest_size == 0) return;
    size_t j = 0;
    for (size_t i = 0; src && src[i] != 0 && j < dest_size - 1; i++) {
      unsigned char c = static_cast<unsigned char>(src[i]);
      if (c >= 32 && c <= 126) {
        dest[j++] = char(c);
      } else if (c >= 0x80) {
        dest[j++] = '#';
        while (src[i + 1] && (src[i + 1] & 0xC0) == 0x80) i++;
      }
    }
    dest[j] = 0;
  }
  virtual void drawTextEllipsized(int x, int y, int max_width, const char* text) {
    char tmp[96];
    copy(tmp, text, sizeof(tmp));
    if (getTextWidth(tmp) <= max_width) {
      setCursor(x, y);
      print(tmp);
      return;
    }
    const char* ellipsis = "...";
    int ellipsis_w = getTextWidth(ellipsis);
    size_t len = std::strlen(tmp);
    while (len > 0 && getTextWidth(tmp) > max_width - ellipsis_w) tmp[--len] = 0;
    if (len + 3 < sizeof(tmp)) std::strcat(tmp, ellipsis);
    setCursor(x, y);
    print(tmp);
  }
  // Flush the framebuffer to the panel.
  virtual void flush() = 0;

 private:
  static void copy(char* dst, const char* src, size_t max) {
    size_t i = 0;
    for (; src && src[i] && i < max - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
  }
};

}  // namespace corefw::ui
