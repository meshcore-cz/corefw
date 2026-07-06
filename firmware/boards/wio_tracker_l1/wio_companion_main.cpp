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

#include <corefw/companion/Storage.h>

#include "NRF52BLETransport.h"
#include "NRF52FileStore.h"
#include "NRF52USBSerialTransport.h"
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
static board::NRF52USBSerialTransport g_usb;
static ui::SH1106Display g_display;
static ui::ArduinoBuzzer g_buzzer(PIN_BUZZER);
static Dispatcher* g_dispatcher = nullptr;
static Kernel g_kernel;
static proto::LocalIdentity g_identity;
static board::NRF52FileStore g_fs;
static companion::PersistentStore g_store(g_fs);
static CompanionModule* g_companion = nullptr;  // resolved in setup()
static companion::CompanionTransport* g_transport = nullptr;
static bool g_radio_ok = false;

static void applyProfileRadioDefaults(companion::CompanionState& state) {
#ifdef LORA_FREQ
  state.freq_khz = uint32_t(float(LORA_FREQ) * 1000.0f + 0.5f);
#endif
#ifdef LORA_BW
  state.bw_hz = uint32_t(float(LORA_BW) * 1000.0f + 0.5f);
#endif
#ifdef LORA_SF
  state.sf = LORA_SF;
#endif
#ifdef LORA_CR
  state.cr = LORA_CR;
#endif
#ifdef LORA_TX_POWER
  state.tx_power_dbm = LORA_TX_POWER;
#endif
}

static RadioConfig makeRadioConfig(const companion::CompanionState& state) {
  RadioConfig cfg;
  cfg.frequency_mhz = float(state.freq_khz) / 1000.0f;
  cfg.bandwidth_khz = float(state.bw_hz) / 1000.0f;
  cfg.spreading_factor = state.sf;
  cfg.coding_rate = state.cr;
  cfg.tx_power_dbm = state.tx_power_dbm;
  return cfg;
}

// CompanionHost implementation: device services (RTC, battery, persistence).
// Persistence flows through the byte-compatible PersistentStore, so writes land
// in the exact MeshCore file formats.
class WioCompanionHost : public companion::CompanionHost {
 public:
  uint32_t rtcNow() override { return g_rtc.now(); }
  void setRtc(uint32_t s) override { g_rtc.set(s); }
  uint16_t batteryMilliVolts() override {
    return g_kernel.board() ? g_kernel.board()->batteryMilliVolts() : 0;
  }
  uint32_t storageUsedKb() override { return g_fs.usedKb(); }
  uint32_t storageTotalKb() override { return g_fs.totalKb(); }
  const char* manufacturerName() override {
    return g_kernel.board() ? g_kernel.board()->manufacturerName() : "Seeed Studio";
  }
  void savePrefs() override { if (g_companion) g_store.savePrefs(g_companion->state()); }
  void saveContacts() override { if (g_companion) g_store.saveContacts(g_companion->state()); }
  void saveChannels() override { if (g_companion) g_store.saveChannels(g_companion->state()); }
  void applyRadioParams() override {
    if (!g_companion || !g_dispatcher) return;
    g_dispatcher->configureRadio(makeRadioConfig(g_companion->state()));
  }
  void applyTxPower() override { applyRadioParams(); }
  void radioStats(int16_t& noise_floor, int8_t& last_rssi, int8_t& last_snr_q4,
                  uint32_t& tx_air_s, uint32_t& rx_air_s) override {
    noise_floor = g_radio_ok ? -120 : 0;
    RadioDriver* radio = g_kernel.board() ? g_kernel.board()->radio() : nullptr;
    last_rssi = radio ? int8_t(radio->lastRSSI()) : 0;
    last_snr_q4 = radio ? int8_t(radio->lastSNR() * 4.0f) : 0;
    tx_air_s = 0;
    rx_air_s = 0;
  }
  bool factoryReset() override {
    // Only an explicit CMD_FACTORY_RESET erases stored data.
    g_fs.remove(companion::PREFS_FILE);
    g_fs.remove(companion::CONTACTS_FILE);
    g_fs.remove(companion::CHANNELS_FILE);
    return true;
  }
};

static WioCompanionHost g_host;

// set the route bits on a packet built by the datagram builders (which leave
// route = 0), preserving the payload type.
static void setRoute(proto::Packet& pkt, uint8_t route) {
  pkt.header = uint8_t((pkt.header & ~proto::PH_ROUTE_MASK) | (route & proto::PH_ROUTE_MASK));
}

// WioMeshSender bridges the companion command handler to the radio scheduler.
// Flood packets get ROUTE_FLOOD; packets with a known return path go ROUTE_DIRECT
// with the path attached. The Dispatcher owns airtime/duty-cycle scheduling.
class WioMeshSender : public companion::MeshSender {
 public:
  int sendToContact(proto::Packet& pkt, const companion::ContactInfo& c,
                    uint32_t& est_timeout) override {
    if (!g_dispatcher) return companion::SEND_FAILED;
    if (c.out_path_len == companion::OUT_PATH_UNKNOWN) {
      setRoute(pkt, proto::ROUTE_FLOOD);
      pkt.setPathHashSizeAndCount(1, 0);
      est_timeout = 8000;
      g_dispatcher->send(pkt);
      return companion::SEND_FLOOD;
    }
    setRoute(pkt, proto::ROUTE_DIRECT);
    applyPath(pkt, c.out_path, c.out_path_len);
    est_timeout = 3000;
    g_dispatcher->send(pkt);
    return companion::SEND_DIRECT;
  }
  bool sendGroup(proto::Packet& pkt) override {
    if (!g_dispatcher) return false;
    setRoute(pkt, proto::ROUTE_FLOOD);
    pkt.setPathHashSizeAndCount(1, 0);
    g_dispatcher->send(pkt);
    return true;
  }
  bool sendDirect(proto::Packet& pkt, const uint8_t* path, uint8_t path_len) override {
    if (!g_dispatcher) return false;
    setRoute(pkt, proto::ROUTE_DIRECT);
    applyPath(pkt, path, path_len);
    g_dispatcher->send(pkt);
    return true;
  }
  bool sendZeroHop(proto::Packet& pkt) override {
    if (!g_dispatcher) return false;
    setRoute(pkt, proto::ROUTE_FLOOD);
    pkt.setPathHashSizeAndCount(1, 0);
    g_dispatcher->send(pkt);
    return true;
  }
  bool sendRawPacket(const uint8_t* wire, size_t len, uint8_t /*priority*/) override {
    if (!g_dispatcher) return false;
    proto::Packet pkt;
    if (!pkt.readFrom(wire, len)) return false;
    g_dispatcher->send(pkt);
    return true;
  }
  bool sendSelfAdvert(bool flood) override {
    if (!g_dispatcher) return false;
    proto::AdvertData ad;
    ad.type = proto::ADV_TYPE_CHAT;
    proto::Packet pkt;
    if (!proto::buildAdvert(pkt, g_identity, g_rtc.now(), ad)) return false;
    setRoute(pkt, proto::ROUTE_FLOOD);
    if (!flood) pkt.setPathHashSizeAndCount(1, 0);  // zero-hop
    g_dispatcher->send(pkt);
    return true;
  }
  uint32_t rtcNowUnique() override { return g_rtc.now() + (seq_++ & 0x3); }
  uint32_t random32() override { rng_ = rng_ * 1664525u + 1013904223u; return rng_; }

 private:
  static void applyPath(proto::Packet& pkt, const uint8_t* path, uint8_t path_len) {
    uint8_t n = path_len & 63;
    pkt.setPathHashSizeAndCount(1, n);
    memcpy(pkt.path, path, n);
  }
  uint32_t seq_ = 0;
  uint32_t rng_ = 0xC0FFEE11;
};

static WioMeshSender g_sender;

// Minimal Wio joystick handling for the MeshCore-style companion pages. The
// button pins are active-low in the board variant.
struct ButtonEdge {
  uint8_t pin;
  bool last = true;
  uint32_t changed_at = 0;
  bool began = false;

  void begin() {
    pinMode(pin, INPUT_PULLUP);
    last = digitalRead(pin);
    changed_at = millis();
    began = true;
  }

  bool pressed() {
    if (!began) begin();
    bool now = digitalRead(pin);
    uint32_t t = millis();
    if (now != last && t - changed_at > 40) {
      last = now;
      changed_at = t;
      return now == LOW;
    }
    return false;
  }
};

#if defined(PIN_BUTTON4) && defined(PIN_BUTTON5) && defined(PIN_BUTTON6)
static ButtonEdge g_btn_left{PIN_BUTTON4};
static ButtonEdge g_btn_right{PIN_BUTTON5};
static ButtonEdge g_btn_enter{PIN_BUTTON6};
#endif

// findCompanion locates the CompanionModule the generated composition
// registered, so we can attach the board's transport/display/buzzer to it.
static CompanionModule* findCompanion() {
  for (int i = 0; i < g_kernel.moduleCount(); ++i) {
    Module* m = g_kernel.module(i);
    if (strcmp(m->name(), "companion") == 0) return static_cast<CompanionModule*>(m);
  }
  return nullptr;
}

static void showStage(const char* line1, const char* line2 = "") {
  if (!g_display.isOn()) return;
  g_display.startFrame();
  g_display.setTextSize(1);
  g_display.setColor(ui::Display::LIGHT);
  g_display.setCursor(0, 0);
  g_display.print(line1);
  g_display.setCursor(0, 14);
  g_display.print(line2);
  g_display.endFrame();
}

void setup() {
  // 1. Bring up the board (buses, radio, power rails) via the composition root.
  corefw_compose(g_kernel);
  if (g_kernel.board()) g_kernel.board()->begin();
  g_companion = findCompanion();

  // Bring up the display early so every potentially blocking startup stage has
  // a visible breadcrumb on the OLED.
  bool display_ok = g_display.begin();
  if (display_ok) {
    showStage("corefw display", "Wio Tracker L1");
    delay(500);
  }

  // 2. Mount the existing filesystem (never formats) and load identity, prefs,
  //    contacts and channels in MeshCore's on-flash formats. A device reflashed
  //    from MeshCore keeps its identity (mesh address) and data. Only a fresh
  //    device generates a new identity here (seeded from the hardware RNG).
  showStage("startup", "storage");
  g_fs.begin();
  if (g_companion) {
    applyProfileRadioDefaults(g_companion->state());
    uint8_t seed[proto::SEED_SIZE];
    for (size_t i = 0; i < sizeof(seed); i++) seed[i] = uint8_t(random(256));
    g_store.loadAll(g_companion->state(), seed);
    g_identity = g_companion->state().self;
  }

  // 3. Build the radio scheduler around the board's configured radio.
  showStage("startup", "radio");
  RadioDriver* radio = g_kernel.board() ? g_kernel.board()->radio() : nullptr;
  RadioConfig cfg = g_companion ? makeRadioConfig(g_companion->state()) : RadioConfig{};
  static Dispatcher dispatcher(radio, &g_clock, g_identity.pub_key, cfg);
  g_dispatcher = &dispatcher;
  if (radio) {
    g_radio_ok = radio->begin(cfg);
    if (!g_radio_ok) showStage("radio init", "failed");
  }
  // Deliver received packets to the companion, which decrypts those addressed
  // to this node and surfaces messages/adverts to the app.
  if (g_companion) dispatcher.subscribe(g_companion);

  // 4. Peripherals for the companion experience.
  showStage("startup", "buzzer");
  g_buzzer.begin();
  if (g_companion && g_companion->transportKind() == CompanionTransportKind::USB) {
    showStage("startup", "usb serial");
    g_usb.begin(115200);
    g_transport = &g_usb;
  } else {
    showStage("startup", "ble");
    g_ble.begin(/*name=*/"corefw-wio", BLE_PIN_CODE);
    g_transport = &g_ble;
  }

  // 5. Attach everything to the companion module and start the kernel.
  if (g_companion) {
    CompanionModule* comp = g_companion;
    comp->attachClock(&g_clock);
    comp->attachHost(&g_host);
    comp->attachSender(&g_sender);
    comp->attachTransport(g_transport);
    comp->attachDisplay(&g_display);
    comp->attachBuzzer(&g_buzzer);
  }

#if defined(PIN_BUTTON4) && defined(PIN_BUTTON5) && defined(PIN_BUTTON6)
  g_btn_left.begin();
  g_btn_right.begin();
  g_btn_enter.begin();
#endif

  static struct : PowerCoordinator {
    void requireRadioUntil(uint64_t) override {}
    void scheduleWake(uint64_t) override {}
    void preventDeepSleep(const char*) override {}
    void releaseDeepSleep(const char*) override {}
  } power;
  showStage("startup", "kernel");
  g_kernel.begin(dispatcher, power);
  showStage("startup", "loop");
}

void loop() {
  const uint32_t now = g_clock.millis();
  if (g_dispatcher) g_dispatcher->loop();

  // Reflect BLE connection state as kernel events, then pump the companion.
  static bool wasConnected = false;
  if (g_companion) {
#if defined(PIN_BUTTON4) && defined(PIN_BUTTON5) && defined(PIN_BUTTON6)
    if (g_btn_left.pressed()) g_companion->ui().prevPage();
    if (g_btn_right.pressed()) g_companion->ui().nextPage();
    if (g_btn_enter.pressed()) g_companion->ui().clearMessagePreview();
#endif
    bool nowConnected = g_transport ? g_transport->connected() : false;
    if (nowConnected != wasConnected) {
      Event e;
      e.type = nowConnected ? EventType::CompanionConnected : EventType::CompanionDisconnected;
      g_kernel.dispatch(e);
      wasConnected = nowConnected;
    }
    g_companion->tick(now);
  }
}

#endif  // COREFW_TARGET && NRF52_PLATFORM
