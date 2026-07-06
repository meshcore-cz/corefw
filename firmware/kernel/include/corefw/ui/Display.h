// Display abstraction.
//
// A minimal text/graphics surface the UI draws onto, so screen logic is
// board-agnostic and host-testable. Concrete drivers (SH1106 on the Wio Tracker
// L1, SSD1306 on the Heltec V3) implement this over their controller library.
#pragma once

namespace corefw::ui {

class Display {
 public:
  virtual ~Display() = default;

  virtual int width() const { return 128; }
  virtual int height() const { return 64; }

  virtual void clear() = 0;
  virtual void setCursor(int x, int y) = 0;
  virtual void print(const char* text) = 0;
  // Draw a horizontal separator at row y.
  virtual void hline(int y) { (void)y; }
  // Flush the framebuffer to the panel.
  virtual void flush() = 0;
};

}  // namespace corefw::ui
