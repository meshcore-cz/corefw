// Wio Tracker L1 companion — target entrypoint.
//
// TARGET-ONLY reference wiring (COREFW_TARGET && NRF52_PLATFORM). This is the
// concrete Arduino setup()/loop() that turns a composed corefw image into a
// working Wio Tracker L1 companion: SX1262 radio + BLE transport + SH1106 OLED +
// piezo buzzer. The kernel owns scheduling/power; this file only constructs the
// board peripherals and hands them to the CompanionModule the generated
// composition root registered.
//
// It cannot be built by the host test suite (it needs the Adafruit nRF52 core,
// RadioLib and Bluefruit); the portable logic it drives is covered by the host
// tests under firmware/kernel/*.
#include <corefw/Kernel.h>

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM)

#include <Arduino.h>

#include <corefw/modules/CompanionModule.h>
#include <corefw/protocol/Advert.h>
#include <corefw/runtime/Clock.h>
#include <corefw/runtime/Dispatcher.h>

#include "NRF52BLETransport.h"
// These live outside the kernel include root; the generated build adds -I firmware.
#include <drivers/buzzer/ArduinoBuzzer.h>
#include <drivers/display/sh1106/SH1106Display.h>

#ifndef PIN_BUZZER
#define PIN_BUZZER 12
#endif
#ifndef BLE_PIN_CODE
#define BLE_PIN_CODE 0
#endif

using namespace corefw;

// --- Board services -------------------------------------------------------

// ArduinoClock is the kernel's monotonic time source on device.
class ArduinoClock : public Clock {
 public:
  uint32_t millis() const override { return ::millis(); }
};

// SoftRTC keeps wall-clock seconds; the app sets it via CMD_SET_DEVICE_TIME.
class SoftRTC {
 public:
  uint32_t now() const { return base_ + (::millis() - set_at_) / 1000; }
  void set(uint32_t secs) {
    base_ = secs;
    set_at_ = ::millis();
  }
 private:
  uint32_t base_ = 0;
  uint32_t set_at_ = 0;
};

// Globals wired in setup().
static ArduinoClock g_clock;
static SoftRTC g_rtc;
static board::NRF52BLETransport g_ble;
static ui::SH1106Display g_display;
static ui::ArduinoBuzzer g_buzzer(PIN_BUZZER);
static Dispatcher* g_dispatcher = nullptr;
static Kernel g_kernel;
static proto::LocalIdentity g_identity;

// CompanionHost implementation backed by the board + RTC + mesh.
class WioCompanionHost : public companion::CompanionHost {
 public:
  uint32_t rtcNow() override { return g_rtc.now(); }
  void setRtc(uint32_t s) override { g_rtc.set(s); }
  uint16_t batteryMilliVolts() override {
    return g_kernel.board() ? g_kernel.board()->batteryMilliVolts() : 0;
  }
  const char* manufacturerName() override {
    return g_kernel.board() ? g_kernel.board()->manufacturerName() : "Seeed Studio";
  }
  void sendSelfAdvert(bool flood) override {
    if (!g_dispatcher) return;
    proto::AdvertData ad;
    ad.type = proto::ADV_TYPE_CHAT;
    // Name is carried by the companion state; keep the advert minimal here.
    proto::Packet pkt;
    if (proto::buildAdvert(pkt, g_identity, g_rtc.now(), ad)) {
      if (!flood) pkt.setPathHashSizeAndCount(1, 0);  // zero-hop
      g_dispatcher->send(pkt);
    }
  }
};

static WioCompanionHost g_host;

// findCompanion locates the CompanionModule the generated composition
// registered, so we can attach the board's transport/display/buzzer to it.
static CompanionModule* findCompanion() {
  for (int i = 0; i < g_kernel.moduleCount(); ++i) {
    Module* m = g_kernel.module(i);
    if (strcmp(m->name(), "companion") == 0) return static_cast<CompanionModule*>(m);
  }
  return nullptr;
}

void setup() {
  // 1. Bring up the board (buses, radio, power rails) via the composition root.
  corefw_compose(g_kernel);
  if (g_kernel.board()) g_kernel.board()->begin();

  // 2. Load or create this node's identity (persisted by the board's storage;
  //    a fresh device generates one).
  //    ... storage load elided in this reference ...

  // 3. Build the radio scheduler around the board's configured radio.
  RadioDriver* radio = g_kernel.board() ? g_kernel.board()->radio() : nullptr;
  RadioConfig cfg;
  cfg.tx_power_dbm = int8_t(g_kernel.powerPolicy() ? 22 : 22);
  static Dispatcher dispatcher(radio, &g_clock, g_identity.pub_key, cfg);
  g_dispatcher = &dispatcher;
  if (radio) radio->begin(cfg);

  // 4. Peripherals for the companion experience.
  g_display.begin();
  g_buzzer.begin();
  g_ble.begin(/*name=*/"corefw-wio", BLE_PIN_CODE);

  // 5. Attach everything to the companion module and start the kernel.
  if (CompanionModule* comp = findCompanion()) {
    memcpy(comp->state().self_pub, g_identity.pub_key, proto::PUB_KEY_SIZE);
    comp->attachClock(&g_clock);
    comp->attachHost(&g_host);
    comp->attachTransport(&g_ble);
    comp->attachDisplay(&g_display);
    comp->attachBuzzer(&g_buzzer);
  }

  static struct : PowerCoordinator {
    void requireRadioUntil(uint64_t) override {}
    void scheduleWake(uint64_t) override {}
    void preventDeepSleep(const char*) override {}
    void releaseDeepSleep(const char*) override {}
  } power;
  g_kernel.begin(dispatcher, power);
}

void loop() {
  const uint32_t now = g_clock.millis();
  if (g_dispatcher) g_dispatcher->loop();

  // Reflect BLE connection state as kernel events, then pump the companion.
  static bool wasConnected = false;
  if (CompanionModule* comp = findCompanion()) {
    bool nowConnected = g_ble.connected();
    if (nowConnected != wasConnected) {
      Event e{nowConnected ? EventType::CompanionConnected : EventType::CompanionDisconnected};
      g_kernel.dispatch(e);
      wasConnected = nowConnected;
    }
    comp->tick(now);
  }
}

#endif  // COREFW_TARGET && NRF52_PLATFORM
