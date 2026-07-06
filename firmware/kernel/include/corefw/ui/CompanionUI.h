// CompanionUI — the on-device OLED screen for a companion node.
//
// Portable screen-state model: it holds what should be shown (node name,
// connection state, battery, unread messages, a status line) and renders it onto
// any Display. Keeping the logic here (not in the driver) makes it host-testable
// and reusable across the SH1106 (Wio Tracker L1) and SSD1306 (Heltec V3) panels.
#pragma once

#include <corefw/ui/Display.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace corefw::ui {

class CompanionUI {
 public:
  void setNodeName(const char* n) { copy(node_name_, n, sizeof(node_name_)); }
  void setConnected(bool c) { connected_ = c; }
  void setBatteryMilliVolts(uint16_t mv) { batt_mv_ = mv; }
  void setUnread(int n) { unread_ = n; }
  void setStatus(const char* s) { copy(status_, s, sizeof(status_)); }
  void setLastMessage(const char* s) { copy(last_msg_, s, sizeof(last_msg_)); }

  bool connected() const { return connected_; }
  int unread() const { return unread_; }

  // render draws the current state. Layout:
  //   line 0: node name              (header)
  //   ------- separator
  //   line 1: BLE state + battery
  //   line 2: unread count
  //   line 3: last message preview / status
  void render(Display& d) const {
    d.clear();
    d.setCursor(0, 0);
    d.print(node_name_);
    d.hline(10);

    char line[48];

    d.setCursor(0, 14);
    std::snprintf(line, sizeof(line), "BLE:%s  %u.%02uV", connected_ ? "on" : "off",
                  batt_mv_ / 1000, (batt_mv_ % 1000) / 10);
    d.print(line);

    d.setCursor(0, 26);
    std::snprintf(line, sizeof(line), "Unread: %d", unread_);
    d.print(line);

    d.setCursor(0, 40);
    d.print(last_msg_[0] ? last_msg_ : status_);

    d.flush();
  }

 private:
  static void copy(char* dst, const char* src, size_t max) {
    size_t i = 0;
    for (; src && src[i] && i < max - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
  }

  char node_name_[32] = "corefw";
  char status_[48] = "Ready";
  char last_msg_[48] = "";
  bool connected_ = false;
  uint16_t batt_mv_ = 0;
  int unread_ = 0;
};

}  // namespace corefw::ui
