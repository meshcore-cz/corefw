// CompanionModule — the companion role: bridges a phone/app to the mesh.
//
// It ties together the portable, host-tested Companion Protocol pieces: the
// frame codec, the command handler, the OLED UI and the buzzer. The transport
// (BLE/USB/TCP), display panel, buzzer output and device services (RTC,
// battery, advert sending) are provided by the board at startup — this class
// owns the orchestration, which is why it stays board-agnostic and testable.
#pragma once

#include <corefw/Mesh.h>
#include <corefw/Module.h>
#include <corefw/companion/Commands.h>
#include <corefw/companion/FrameCodec.h>
#include <corefw/companion/Transport.h>
#include <corefw/runtime/Clock.h>
#include <corefw/ui/Buzzer.h>
#include <corefw/ui/CompanionUI.h>
#include <cstring>

namespace corefw {

enum class CompanionTransport { BLE, USB, WiFi };

class CompanionModule : public Module {
 public:
  const char* name() const override { return "companion"; }

  // --- Configuration (from generated code) --------------------------------
  void setTransport(const char* t) {
    if (std::strcmp(t, "usb") == 0) transport_kind_ = CompanionTransport::USB;
    else if (std::strcmp(t, "wifi") == 0) transport_kind_ = CompanionTransport::WiFi;
    else transport_kind_ = CompanionTransport::BLE;
  }
  CompanionTransport transportKind() const { return transport_kind_; }

  // --- Board-provided services (wired by the target main) -----------------
  void attachTransport(companion::CompanionTransport* io) { io_ = io; }
  void attachHost(companion::CompanionHost* host) { host_ = host; }
  void attachClock(Clock* clk) { clock_ = clk; }
  void attachDisplay(ui::Display* d) { display_ = d; }
  void attachBuzzer(ui::ToneOutput* b) {
    buzzer_ = b;
    melody_.setOutput(b);
  }
  void configureState(const companion::CompanionState& s) { state_ = s; }

  companion::CompanionState& state() { return state_; }
  ui::CompanionUI& ui() { return ui_; }

  // --- Lifecycle ----------------------------------------------------------
  void initialize(Context& ctx) override {
    ctx_ = &ctx;
    ui_.setNodeName(state_.node_name);
  }

  // tick pumps the transport, command handling, melody and screen. The target
  // calls this from the main loop; `now_ms` is the monotonic clock.
  void tick(uint32_t now_ms) {
    pumpTransport();
    if (melody_.playing()) melody_.loop(now_ms);
    refreshUI(now_ms);
  }

  // Notify the module that a text message arrived from the mesh: bump unread,
  // update the screen and beep.
  void onMeshMessage(const char* preview) {
    unread_++;
    ui_.setUnread(unread_);
    if (preview) ui_.setLastMessage(preview);
    beep(ui::melodies::kMessage);
  }

  void onEvent(const Event& e) override {
    if (e.type == EventType::CompanionConnected) {
      connected_ = true;
      ui_.setConnected(true);
      beep(ui::melodies::kStartup);
    } else if (e.type == EventType::CompanionDisconnected) {
      connected_ = false;
      ui_.setConnected(false);
    }
  }

  bool connected() const { return connected_; }

 private:
  // Read inbound bytes, decode frames, run the command handler and reply.
  void pumpTransport() {
    if (io_ == nullptr || host_ == nullptr) return;
    uint8_t in[companion::MAX_FRAME_SIZE];
    size_t n = io_->read(in, sizeof(in));
    if (n == 0) return;
    companion::CommandHandler handler(state_, *host_);
    decoder_.feed(in, n, frame_, [&](const uint8_t* payload, size_t plen) {
      uint8_t resp[companion::MAX_FRAME_SIZE];
      size_t rlen = handler.handle(payload, plen, resp);
      if (rlen > 0) {
        uint8_t out[companion::MAX_FRAME_SIZE + 3];
        size_t olen = companion::encodeFrame(out, resp, rlen);
        io_->write(out, olen);
      }
    });
  }

  void refreshUI(uint32_t now_ms) {
    if (display_ == nullptr) return;
    if (now_ms - last_render_ < 250 && !dirty_) return;  // ~4 Hz refresh
    if (host_ != nullptr) ui_.setBatteryMilliVolts(host_->batteryMilliVolts());
    ui_.render(*display_);
    last_render_ = now_ms;
    dirty_ = false;
  }

  void beep(const char* song) {
    if (buzzer_ == nullptr || clock_ == nullptr) return;
    melody_.play(song, clock_->millis());
  }

  Context* ctx_ = nullptr;
  CompanionTransport transport_kind_ = CompanionTransport::BLE;

  companion::CompanionTransport* io_ = nullptr;
  companion::CompanionHost* host_ = nullptr;
  Clock* clock_ = nullptr;
  ui::Display* display_ = nullptr;
  ui::ToneOutput* buzzer_ = nullptr;

  companion::CompanionState state_;
  companion::FrameDecoder decoder_;
  uint8_t frame_[companion::MAX_FRAME_SIZE] = {};

  ui::CompanionUI ui_;
  ui::Melody melody_{nullptr};

  bool connected_ = false;
  int unread_ = 0;
  bool dirty_ = true;
  uint32_t last_render_ = 0;
};

}  // namespace corefw
