// NullDisplay — a no-op Display for headless boards (e.g. the SenseCAP Solar
// repeater, which has no panel).
//
// Every drawing primitive is a no-op and isOn() reports false, so board bring-up
// breadcrumbs (showStage) and the CompanionUI both skip rendering without any
// per-call #ifdefs at the call sites. The unified platform mains instantiate this
// when the board manifest declares no display (DISPLAY_CLASS undefined).
#pragma once

#include <corefw/ui/Display.h>

namespace corefw::ui {

class NullDisplay : public Display {
 public:
  // Matches the concrete panel drivers' begin() so the shared platform main can
  // call g_display.begin() unconditionally; reports "no panel present".
  bool begin() { return false; }

  bool isOn() const override { return false; }
  void clear() override {}
  void setCursor(int, int) override {}
  void print(const char*) override {}
  void flush() override {}
};

}  // namespace corefw::ui
