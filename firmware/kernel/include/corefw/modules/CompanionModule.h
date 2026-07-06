// CompanionModule — pairs the node with a phone/app over the Companion Protocol.
//
// The transport (BLE / USB / Wi-Fi) is selected by configuration. The module
// frames Companion Protocol messages over the chosen transport and bridges them
// to the MeshService; framing stays compatible with the reference companion
// firmware so existing apps connect unchanged.
#pragma once

#include <corefw/Mesh.h>
#include <corefw/Module.h>
#include <corefw/companion/FrameCodec.h>
#include <cstring>

namespace corefw {

enum class CompanionTransport { BLE, USB, WiFi };

class CompanionModule : public Module {
 public:
  const char* name() const override { return "companion"; }

  void setTransport(const char* t) {
    if (std::strcmp(t, "usb") == 0) transport_ = CompanionTransport::USB;
    else if (std::strcmp(t, "wifi") == 0) transport_ = CompanionTransport::WiFi;
    else transport_ = CompanionTransport::BLE;
  }

  CompanionTransport transport() const { return transport_; }

  void initialize(Context& ctx) override { ctx_ = &ctx; }

  void onEvent(const Event& e) override {
    if (e.type == EventType::CompanionConnected) connected_ = true;
    if (e.type == EventType::CompanionDisconnected) connected_ = false;
  }

  bool connected() const { return connected_; }

  // Feed raw bytes from the active transport (UART/BLE/TCP). Complete Companion
  // Protocol frames are dispatched to handleCommand(); the codec is byte-
  // compatible with the reference companion firmware.
  void onTransportBytes(const uint8_t* data, size_t len) {
    decoder_.feed(data, len, scratch_,
                  [this](const uint8_t* payload, size_t n) { handleCommand(payload, n); });
  }

  // handleCommand processes one decoded inbound frame (payload[0] is the command
  // code). Full command coverage is layered on top of this hook; the transport,
  // framing and codes are already compatible.
  virtual void handleCommand(const uint8_t* payload, size_t len) { (void)payload; (void)len; }

 protected:
  // sendFrame emits a device->app frame over the active transport. Subclasses
  // provide writeTransport(); this handles the '>' + len16 framing.
  size_t buildFrame(uint8_t* out, const uint8_t* payload, size_t len) const {
    return companion::encodeFrame(out, payload, len);
  }

 private:
  Context* ctx_ = nullptr;
  CompanionTransport transport_ = CompanionTransport::BLE;
  bool connected_ = false;
  companion::FrameDecoder decoder_;
  uint8_t scratch_[companion::MAX_FRAME_SIZE] = {};
};

}  // namespace corefw
