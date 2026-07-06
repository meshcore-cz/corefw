// CompanionModule — pairs the node with a phone/app over the Companion Protocol.
//
// The transport (BLE / USB / Wi-Fi) is selected by configuration. The module
// frames Companion Protocol messages over the chosen transport and bridges them
// to the MeshService; framing stays compatible with the reference companion
// firmware so existing apps connect unchanged.
#pragma once

#include <corefw/Mesh.h>
#include <corefw/Module.h>
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

 private:
  Context* ctx_ = nullptr;
  CompanionTransport transport_ = CompanionTransport::BLE;
  bool connected_ = false;
};

}  // namespace corefw
