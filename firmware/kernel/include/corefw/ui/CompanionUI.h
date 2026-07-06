// CompanionUI — MeshCore-style OLED UI for a companion node.
//
// This is a portable screen-state model inspired by MeshCore's
// examples/companion_radio/ui-new. It deliberately contains no Arduino, radio or
// board calls: the module feeds it connection, radio, battery, message and advert
// state, then asks it to render onto any Display.
#pragma once

#include <corefw/ui/Display.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace corefw::ui {

class CompanionUI {
 public:
  enum class Page : uint8_t { Home = 0, Recent, Radio, Gps, Bluetooth, Advert, Shutdown, Count };

  void begin(uint32_t now_ms) {
    started_ms_ = now_ms;
    show_splash_ = true;
    dirty_ = true;
  }

  void setNodeName(const char* n) { copy(node_name_, n, sizeof(node_name_)); dirty_ = true; }
  void setConnected(bool c) { if (connected_ != c) { connected_ = c; dirty_ = true; } }
  void setSerialEnabled(bool e) { if (serial_enabled_ != e) { serial_enabled_ = e; dirty_ = true; } }
  void setBatteryMilliVolts(uint16_t mv) { if (batt_mv_ != mv) { batt_mv_ = mv; dirty_ = true; } }
  void setUnread(int n) { if (unread_ != n) { unread_ = n; dirty_ = true; } }
  void setBlePin(uint32_t pin) { if (ble_pin_ != pin) { ble_pin_ = pin; dirty_ = true; } }
  void setRadio(uint32_t freq_khz, uint32_t bw_hz, uint8_t sf, uint8_t cr, int8_t tx_dbm) {
    freq_khz_ = freq_khz;
    bw_hz_ = bw_hz;
    sf_ = sf;
    cr_ = cr;
    tx_dbm_ = tx_dbm;
    dirty_ = true;
  }
  void setNoiseFloorDbm(int16_t dbm) {
    if (noise_floor_dbm_ != dbm) {
      noise_floor_dbm_ = dbm;
      dirty_ = true;
    }
  }
  void setGps(bool enabled, bool fix, uint8_t sats, int32_t lat_e6, int32_t lon_e6) {
    if (gps_enabled_ != enabled || gps_fix_ != fix || gps_sats_ != sats ||
        gps_lat_e6_ != lat_e6 || gps_lon_e6_ != lon_e6) {
      gps_enabled_ = enabled;
      gps_fix_ = fix;
      gps_sats_ = sats;
      gps_lat_e6_ = lat_e6;
      gps_lon_e6_ = lon_e6;
      dirty_ = true;
    }
  }
  void setStatus(const char* s) { copy(status_, s, sizeof(status_)); dirty_ = true; }
  void setLastMessage(const char* s) { copy(last_msg_, s, sizeof(last_msg_)); dirty_ = true; }

  bool connected() const { return connected_; }
  int unread() const { return unread_; }
  bool dirty() const { return dirty_; }
  Page page() const { return page_; }

  void nextPage() {
    show_splash_ = false;
    // A visible message preview swallows the first press so a single-button
    // board (e.g. Heltec V3) can dismiss it; the next press then navigates.
    if (dismissPreview()) return;
    page_ = Page((uint8_t(page_) + 1) % uint8_t(Page::Count));
    dirty_ = true;
  }
  void prevPage() {
    show_splash_ = false;
    if (dismissPreview()) return;
    page_ = Page((uint8_t(page_) + uint8_t(Page::Count) - 1) % uint8_t(Page::Count));
    dirty_ = true;
  }
  void gotoHome() {
    show_splash_ = false;
    page_ = Page::Home;
    dirty_ = true;
  }

  void showAlert(const char* text, uint32_t now_ms, uint32_t duration_ms = 1000) {
    copy(alert_, text, sizeof(alert_));
    alert_expiry_ms_ = now_ms + duration_ms;
    dirty_ = true;
  }

  void addRecentAdvert(const char* name, uint32_t now_s) {
    if (!name || !name[0]) return;
    for (int i = kRecentCount - 1; i > 0; --i) recent_[i] = recent_[i - 1];
    copy(recent_[0].name, name, sizeof(recent_[0].name));
    recent_[0].seen_s = now_s;
    dirty_ = true;
  }

  void addMessagePreview(uint8_t path_len, const char* from_name, const char* text, uint32_t now_s) {
    MsgPreview& p = previews_[preview_head_];
    p.timestamp_s = now_s;
    if (path_len == 0xFF) {
      std::snprintf(p.origin, sizeof(p.origin), "(D) %s:", from_name ? from_name : "?");
    } else {
      std::snprintf(p.origin, sizeof(p.origin), "(%u) %s:", unsigned(path_len),
                    from_name ? from_name : "?");
    }
    copy(p.text, text, sizeof(p.text));
    preview_head_ = (preview_head_ + 1) % kPreviewCount;
    if (preview_count_ < kPreviewCount) preview_count_++;
    setLastMessage(text);
    page_ = Page::Home;
    showing_preview_ = true;
    show_splash_ = false;
    dirty_ = true;
  }

  void clearMessagePreview() {
    preview_count_ = 0;
    showing_preview_ = false;
    dirty_ = true;
  }

  // Stop showing the preview overlay without discarding message history.
  // Returns true if a preview was actually being shown (i.e. the press was
  // consumed by dismissing it).
  bool dismissPreview() {
    if (!showing_preview_) return false;
    showing_preview_ = false;
    dirty_ = true;
    return true;
  }

  // Render returns the preferred delay before the next refresh.
  uint32_t render(Display& d, uint32_t now_ms, uint32_t now_s) {
    if (show_splash_ && now_ms - started_ms_ >= kSplashMs) {
      show_splash_ = false;
      dirty_ = true;
    }

    d.startFrame(Display::DARK);
    if (show_splash_) {
      renderSplash(d);
    } else if (showing_preview_ && preview_count_ > 0) {
      renderMsgPreview(d, now_s);
    } else {
      renderHomeShell(d, now_s);
    }
    if (now_ms < alert_expiry_ms_) renderAlert(d);
    d.endFrame();
    dirty_ = false;

    if (now_ms < alert_expiry_ms_) return alert_expiry_ms_ - now_ms;
    if (show_splash_) return 1000;
    if (showing_preview_) return 1000;
    return 5000;
  }

 private:
  static constexpr uint32_t kSplashMs = 3000;
  static constexpr int kRecentCount = 4;
  static constexpr int kPreviewCount = 8;

  struct RecentAdvert {
    char name[32] = {};
    uint32_t seen_s = 0;
  };
  struct MsgPreview {
    uint32_t timestamp_s = 0;
    char origin[62] = {};
    char text[78] = {};
  };

  void renderSplash(Display& d) const {
    d.setColor(Display::BLUE);
    d.setTextSize(2);
    d.drawTextCentered(d.width() / 2, 4, "meshcore");
    d.setColor(Display::LIGHT);
    d.setTextSize(1);
    d.drawTextCentered(d.width() / 2, 25, "https://meshcore.io");
    d.drawTextCentered(d.width() / 2, 40, "corefw");
    d.drawTextCentered(d.width() / 2, 52, "Wio Tracker L1");
  }

  void renderHomeShell(Display& d, uint32_t now_s) {
    char filtered[sizeof(node_name_)];
    d.translateUTF8ToBlocks(filtered, node_name_, sizeof(filtered));
    d.setTextSize(1);
    d.setColor(Display::GREEN);
    d.drawTextEllipsized(0, 0, d.width() - 35, filtered);
    renderBatteryIndicator(d);
    renderPageDots(d);

    switch (page_) {
      case Page::Home: renderHome(d); break;
      case Page::Recent: renderRecent(d, now_s); break;
      case Page::Radio: renderRadio(d); break;
      case Page::Gps: renderGps(d); break;
      case Page::Bluetooth: renderBluetooth(d); break;
      case Page::Advert: renderAdvert(d); break;
      case Page::Shutdown: renderShutdown(d); break;
      case Page::Count: break;
    }
  }

  void renderHome(Display& d) const {
    char tmp[48];
    d.setColor(Display::YELLOW);
    d.setTextSize(2);
    std::snprintf(tmp, sizeof(tmp), "MSG: %d", unread_);
    d.drawTextCentered(d.width() / 2, 20, tmp);

    d.setTextSize(1);
    if (connected_) {
      d.setColor(Display::GREEN);
      d.drawTextCentered(d.width() / 2, 43, "< Connected >");
    } else if (ble_pin_ != 0) {
      d.setColor(Display::RED);
      std::snprintf(tmp, sizeof(tmp), "Pin:%lu", static_cast<unsigned long>(ble_pin_));
      d.drawTextCentered(d.width() / 2, 43, tmp);
    } else {
      d.setColor(Display::LIGHT);
      d.drawTextCentered(d.width() / 2, 43, status_);
    }
    if (last_msg_[0]) {
      d.setColor(Display::LIGHT);
      d.drawTextEllipsized(0, 54, d.width(), last_msg_);
    }
  }

  void renderRecent(Display& d, uint32_t now_s) const {
    d.setColor(Display::GREEN);
    int y = 20;
    bool any = false;
    for (int i = 0; i < kRecentCount; ++i, y += 11) {
      if (!recent_[i].name[0]) continue;
      any = true;
      char age[12];
      formatAge(age, sizeof(age), now_s, recent_[i].seen_s);
      int age_w = d.getTextWidth(age);
      char filtered[sizeof(recent_[i].name)];
      d.translateUTF8ToBlocks(filtered, recent_[i].name, sizeof(filtered));
      d.drawTextEllipsized(0, y, d.width() - age_w - 2, filtered);
      d.drawTextRightAlign(d.width() - 1, y, age);
    }
    if (!any) {
      d.setColor(Display::LIGHT);
      d.drawTextCentered(d.width() / 2, 32, "No adverts yet");
    }
  }

  void renderRadio(Display& d) const {
    char tmp[48];
    d.setColor(Display::YELLOW);
    d.setTextSize(1);
    d.setCursor(0, 20);
    std::snprintf(tmp, sizeof(tmp), "FQ: %03lu.%03lu SF: %u",
                  static_cast<unsigned long>(freq_khz_ / 1000),
                  static_cast<unsigned long>(freq_khz_ % 1000), unsigned(sf_));
    d.print(tmp);
    d.setCursor(0, 31);
    std::snprintf(tmp, sizeof(tmp), "BW: %lu.%02lu   CR: %u",
                  static_cast<unsigned long>(bw_hz_ / 1000),
                  static_cast<unsigned long>((bw_hz_ % 1000) / 10), unsigned(cr_));
    d.print(tmp);
    d.setCursor(0, 42);
    std::snprintf(tmp, sizeof(tmp), "TX: %ddBm", int(tx_dbm_));
    d.print(tmp);
    d.setCursor(0, 53);
    if (noise_floor_dbm_ == 0) {
      d.print("Noise floor: n/a");
    } else {
      std::snprintf(tmp, sizeof(tmp), "Noise floor: %d", int(noise_floor_dbm_));
      d.print(tmp);
    }
  }

  void renderGps(Display& d) const {
    char tmp[48];
    d.setColor(Display::YELLOW);
    d.setTextSize(1);
    d.setCursor(0, 20);
    if (!gps_enabled_) {
      d.print("GPS: off");
      return;
    }
    if (!gps_fix_) {
      std::snprintf(tmp, sizeof(tmp), "GPS: searching (%u)", unsigned(gps_sats_));
      d.print(tmp);
      return;
    }
    std::snprintf(tmp, sizeof(tmp), "GPS fix - %u sats", unsigned(gps_sats_));
    d.print(tmp);
    // Signed micro-degrees printed as a decimal degree value.
    d.setCursor(0, 34);
    printLatLon(d, tmp, sizeof(tmp), "Lat", gps_lat_e6_);
    d.setCursor(0, 45);
    printLatLon(d, tmp, sizeof(tmp), "Lon", gps_lon_e6_);
  }

  static void printLatLon(Display& d, char* tmp, size_t cap, const char* label, int32_t e6) {
    int32_t whole = e6 / 1000000;
    int32_t frac = e6 % 1000000;
    if (frac < 0) frac = -frac;
    std::snprintf(tmp, cap, "%s: %ld.%06ld", label, long(whole), long(frac));
    d.print(tmp);
  }

  void renderBluetooth(Display& d) const {
    d.setColor(serial_enabled_ ? Display::GREEN : Display::RED);
    d.setTextSize(2);
    d.drawTextCentered(d.width() / 2, 21, "BLE");
    d.setTextSize(1);
    d.setColor(Display::LIGHT);
    d.drawTextCentered(d.width() / 2, 44, serial_enabled_ ? "enabled" : "disabled");
    d.drawTextCentered(d.width() / 2, 54, connected_ ? "connected" : "waiting");
  }

  void renderAdvert(Display& d) const {
    d.setColor(Display::GREEN);
    d.setTextSize(2);
    d.drawTextCentered(d.width() / 2, 21, "ADV");
    d.setTextSize(1);
    d.drawTextCentered(d.width() / 2, 53, "advert page");
  }

  void renderShutdown(Display& d) const {
    d.setColor(Display::GREEN);
    d.setTextSize(2);
    d.drawTextCentered(d.width() / 2, 21, "PWR");
    d.setTextSize(1);
    d.drawTextCentered(d.width() / 2, 53, "hibernate page");
  }

  void renderMsgPreview(Display& d, uint32_t now_s) const {
    const int idx = (preview_head_ + kPreviewCount - 1) % kPreviewCount;
    const MsgPreview& p = previews_[idx];
    char tmp[16];
    d.setCursor(0, 0);
    d.setTextSize(1);
    d.setColor(Display::GREEN);
    std::snprintf(tmp, sizeof(tmp), "Unread: %d", unread_);
    d.print(tmp);
    formatAge(tmp, sizeof(tmp), now_s, p.timestamp_s);
    d.drawTextRightAlign(d.width() - 2, 0, tmp);
    d.hline(11);

    char filtered_origin[sizeof(p.origin)];
    d.translateUTF8ToBlocks(filtered_origin, p.origin, sizeof(filtered_origin));
    d.setCursor(0, 14);
    d.setColor(Display::YELLOW);
    d.drawTextEllipsized(0, 14, d.width(), filtered_origin);

    char filtered_msg[sizeof(p.text)];
    d.translateUTF8ToBlocks(filtered_msg, p.text, sizeof(filtered_msg));
    d.setCursor(0, 25);
    d.setColor(Display::LIGHT);
    d.printWordWrap(filtered_msg, d.width());
  }

  void renderBatteryIndicator(Display& d) const {
    int pct = 0;
    if (batt_mv_ > 0) {
      pct = ((int(batt_mv_) - 3000) * 100) / (4200 - 3000);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    const int icon_w = 24;
    const int icon_h = 10;
    const int icon_x = d.width() - icon_w - 5;
    const int icon_y = 0;
    d.setColor(Display::GREEN);
    d.drawRect(icon_x, icon_y, icon_w, icon_h);
    d.fillRect(icon_x + icon_w, icon_y + icon_h / 4, 3, icon_h / 2);
    int fill_w = (pct * (icon_w - 4)) / 100;
    d.fillRect(icon_x + 2, icon_y + 2, fill_w, icon_h - 4);
  }

  void renderPageDots(Display& d) const {
    int y = 14;
    int x = d.width() / 2 - 5 * (int(Page::Count) - 1);
    for (uint8_t i = 0; i < uint8_t(Page::Count); ++i, x += 10) {
      if (i == uint8_t(page_)) d.fillRect(x - 1, y - 1, 3, 3);
      else d.fillRect(x, y, 1, 1);
    }
  }

  void renderAlert(Display& d) const {
    int y = d.height() / 3;
    int p = d.height() / 32;
    d.setTextSize(1);
    d.setColor(Display::DARK);
    d.fillRect(p, y, d.width() - p * 2, y);
    d.setColor(Display::LIGHT);
    d.drawRect(p, y, d.width() - p * 2, y);
    d.drawTextCentered(d.width() / 2, y + p * 3, alert_);
  }

  static void formatAge(char* out, size_t max, uint32_t now_s, uint32_t then_s) {
    uint32_t secs = now_s >= then_s ? now_s - then_s : 0;
    if (secs < 60) std::snprintf(out, max, "%lus", static_cast<unsigned long>(secs));
    else if (secs < 3600) std::snprintf(out, max, "%lum", static_cast<unsigned long>(secs / 60));
    else std::snprintf(out, max, "%luh", static_cast<unsigned long>(secs / 3600));
  }

  static void copy(char* dst, const char* src, size_t max) {
    size_t i = 0;
    for (; src && src[i] && i < max - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
  }

  char node_name_[32] = "corefw";
  char status_[48] = "Ready";
  char last_msg_[78] = "";
  char alert_[80] = "";
  bool connected_ = false;
  bool serial_enabled_ = true;
  bool show_splash_ = true;
  bool showing_preview_ = false;
  bool dirty_ = true;
  uint16_t batt_mv_ = 0;
  int unread_ = 0;
  uint32_t ble_pin_ = 0;
  uint32_t freq_khz_ = 869525;
  uint32_t bw_hz_ = 250000;
  uint8_t sf_ = 11;
  uint8_t cr_ = 5;
  int8_t tx_dbm_ = 22;
  int16_t noise_floor_dbm_ = 0;
  bool gps_enabled_ = false;
  bool gps_fix_ = false;
  uint8_t gps_sats_ = 0;
  int32_t gps_lat_e6_ = 0;
  int32_t gps_lon_e6_ = 0;
  uint32_t started_ms_ = 0;
  uint32_t alert_expiry_ms_ = 0;
  Page page_ = Page::Home;
  RecentAdvert recent_[kRecentCount];
  MsgPreview previews_[kPreviewCount];
  int preview_head_ = 0;
  int preview_count_ = 0;
};

}  // namespace corefw::ui
