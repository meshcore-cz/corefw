// ESP32 target entrypoint — shared by every ESP32 corefw board.
//
// TARGET-ONLY reference wiring (COREFW_TARGET && ESP32_PLATFORM). The one
// concrete Arduino setup()/loop() for the ESP32 family (Heltec V3, …): SX1262
// radio + optional OLED + a USB-serial or NimBLE transport to the phone/desktop
// app. Board differences arrive as -D defines and through the registered Board
// object; the panel is a NullDisplay when the board manifest declares none.
//
// This is the ESP32-S3 sibling of nrf52_target_main.cpp. Because the base ESP32
// boards have no external QSPI flash, storage is a single internal volume, and
// the nRF52-only freeze diagnostics (watchdog/GPREGRET crumbs/HardFault capture)
// are dropped — the ESP32 has its own reset-reason and task watchdog. The
// portable logic this file drives is covered by the host tests under
// firmware/kernel/*.
#include <corefw/Kernel.h>

#if defined(COREFW_TARGET) && defined(ESP32_PLATFORM)

#include <Arduino.h>
#include <cstring>

#include <corefw/modules/CompanionModule.h>
#if COREFW_HAS_REPEATER
#include <RepeaterModule.h>  // self-encapsulated component; copied in when selected
#endif
#include <corefw/protocol/Advert.h>
#include <corefw/runtime/Clock.h>
#include <corefw/runtime/Dispatcher.h>
#include <platform/shared/PlatformTx.h>

#include <corefw/companion/Storage.h>
#include <corefw/companion/Transport.h>

#include "ESP32BLETransport.h"
#include "ESP32FileStore.h"
#include "ESP32USBSerialTransport.h"

// Display: a real panel when the board manifest declares one (DISPLAY_CLASS /
// DISPLAY_HEADER emitted by codegen), otherwise a no-op NullDisplay so a headless
// ESP32 board drives the same code with no panel.
#if defined(DISPLAY_CLASS)
#include DISPLAY_HEADER
namespace { using BoardDisplay = corefw::ui::DISPLAY_CLASS; constexpr bool kHasDisplay = true; }
#else
#include <corefw/ui/NullDisplay.h>
namespace { using BoardDisplay = corefw::ui::NullDisplay; constexpr bool kHasDisplay = false; }
#endif

#ifndef BLE_PIN_CODE
#define BLE_PIN_CODE 0
#endif

using namespace corefw;
using namespace corefw::platform;  // setRoute / applyPath / emitAdvert

// --- Board services -------------------------------------------------------

// ArduinoClock is the kernel's monotonic time source on device.
class ArduinoClock : public Clock {
 public:
  uint32_t millis() const override { return ::millis(); }
};

// SoftRTC keeps wall-clock seconds; the app sets it via CMD_SET_DEVICE_TIME.
// The Heltec V3 has no battery-backed RTC, so time is lost across resets until
// the app sets it again.
class SoftRTC {
 public:
  uint32_t now() const { return base_ + (::millis() - set_at_) / 1000; }
  void set(uint32_t secs) {
    base_ = secs;
    set_at_ = ::millis();
  }
  bool isSet() const { return base_ != 0; }
 private:
  uint32_t base_ = 0;
  uint32_t set_at_ = 0;
};

// Globals wired in setup().
static ArduinoClock g_clock;
static SoftRTC g_rtc;
static board::ESP32BLETransport g_ble;
static board::ESP32USBSerialTransport g_usb;
static BoardDisplay g_display;
static Dispatcher* g_dispatcher = nullptr;
static Kernel g_kernel;
static proto::LocalIdentity g_identity;
static board::ESP32FileStore g_fs;
static companion::PersistentStore g_store(g_fs);
static CompanionModule* g_companion = nullptr;  // resolved in setup()
#if COREFW_HAS_REPEATER
static RepeaterModule* g_repeater = nullptr;    // resolved in setup() when selected
#endif
static companion::CompanionTransport* g_transport = nullptr;
static bool g_radio_ok = false;

static uint32_t activeBlePin(const companion::CompanionState& state, bool has_display) {
  if (state.ble_pin != 0) return state.ble_pin;
  if (BLE_PIN_CODE == 0) return 0;
  if (has_display && BLE_PIN_CODE == 123456) return 100000 + (esp_random() % 900000);
  return BLE_PIN_CODE;
}

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
class HeltecCompanionHost : public companion::CompanionHost {
 public:
  uint32_t rtcNow() override { return g_rtc.now(); }
  void setRtc(uint32_t s) override { g_rtc.set(s); }
  uint16_t batteryMilliVolts() override {
    return g_kernel.board() ? g_kernel.board()->batteryMilliVolts() : 0;
  }
  uint32_t storageUsedKb() override { return g_fs.usedKb(); }
  uint32_t storageTotalKb() override { return g_fs.totalKb(); }
  const char* manufacturerName() override {
    return g_kernel.board() ? g_kernel.board()->manufacturerName() : "Heltec";
  }
  void savePrefs() override { if (g_companion) g_store.savePrefs(g_companion->state()); }
  void saveContacts() override { if (g_companion) g_store.saveContacts(g_companion->state()); }
  void saveChannels() override { if (g_companion) g_store.saveChannels(g_companion->state()); }
  void applyRadioParams() override {
    if (!g_companion || !g_dispatcher) return;
    g_dispatcher->configureRadio(makeRadioConfig(g_companion->state()));
    g_dispatcher->setAllowFloodForward(g_companion->state().client_repeat != 0);
  }
  void applyTxPower() override { applyRadioParams(); }
  void radioStats(int16_t& noise_floor, int8_t& last_rssi, int8_t& last_snr_q4,
                  uint32_t& tx_air_s, uint32_t& rx_air_s) override {
    noise_floor = radioNoiseFloorDbm();
    RadioDriver* radio = g_kernel.board() ? g_kernel.board()->radio() : nullptr;
    last_rssi = radio ? int8_t(radio->lastRSSI()) : 0;
    last_snr_q4 = radio ? int8_t(radio->lastSNR() * 4.0f) : 0;
    tx_air_s = 0;
    rx_air_s = 0;
  }
  int16_t radioNoiseFloorDbm() const override {
    if (!g_radio_ok) return 0;
    RadioDriver* radio = g_kernel.board() ? g_kernel.board()->radio() : nullptr;
    return radio ? radio->noiseFloorDbm() : 0;
  }
  // The base Heltec V3 has no on-board GPS.
  bool gpsEnabled() const override { return false; }
  bool gpsHasFix() const override { return false; }
  uint8_t gpsSatellites() const override { return 0; }
  int advertPath(const uint8_t* pub_key, uint32_t& recv_ts, uint8_t* path) override {
    if (!g_companion) return -1;
    return g_companion->advertPath(pub_key, recv_ts, path);
  }
  // Custom vars fan out to extension modules (e.g. cz-advert-features), so an
  // extension can expose runtime config over CMD_GET/SET_CUSTOM_VAR.
  size_t getCustomVars(char* out, size_t cap) override { return g_kernel.getConfigVars(out, cap); }
  bool setCustomVar(const char* name, const char* value) override {
    return g_kernel.setConfigVar(name, value);
  }
  bool factoryReset() override {
    // Only an explicit CMD_FACTORY_RESET erases stored data.
    g_fs.remove(companion::PREFS_FILE);
    g_fs.remove(companion::CONTACTS_FILE);
    g_fs.remove(companion::CHANNELS_FILE);
    return true;
  }
};

static HeltecCompanionHost g_host;

// Set the route bits on a packet built by the datagram builders (which leave
// route = 0), preserving the payload type.

// HeltecMeshSender bridges the companion command handler to the radio scheduler.
// Flood packets get ROUTE_FLOOD; packets with a known return path go ROUTE_DIRECT
// with the path attached. The Dispatcher owns airtime/duty-cycle scheduling.
class HeltecMeshSender : public companion::MeshSender {
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
    setRoute(pkt, proto::ROUTE_DIRECT);
    pkt.path_len = 0;
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
    return sendAdvert(proto::ADV_TYPE_CHAT,
                      g_companion ? g_companion->state().node_name : "corefw",
                      flood);
  }
#if COREFW_HAS_REPEATER
  bool sendRepeaterAdvert(bool flood) {
    return sendAdvert(proto::ADV_TYPE_REPEATER,
                      g_repeater ? g_repeater->advertName() : "repeater",
                      flood);
  }
#endif
  bool sendAck(const uint8_t* ack, uint8_t ack_len, const companion::ContactInfo& to) override {
    if (!g_dispatcher) return false;
    proto::Packet pkt;
    pkt.header = uint8_t(proto::PAYLOAD_ACK << proto::PH_TYPE_SHIFT);
    std::memcpy(pkt.payload, ack, ack_len);
    pkt.payload_len = ack_len;
    if (to.out_path_len == companion::OUT_PATH_UNKNOWN) {
      setRoute(pkt, proto::ROUTE_FLOOD);
      pkt.setPathHashSizeAndCount(1, 0);
    } else {
      setRoute(pkt, proto::ROUTE_DIRECT);
      applyPath(pkt, to.out_path, to.out_path_len);
    }
    g_dispatcher->send(pkt);
    return true;
  }
  uint32_t rtcNowUnique() override { return g_rtc.now() + (seq_++ & 0x3); }
  uint32_t random32() override { return esp_random(); }

 private:
  bool sendAdvert(uint8_t type, const char* name, bool flood) {
    if (!g_dispatcher || !g_companion) return false;
    const companion::CompanionState& st = g_companion->state();
    proto::AdvertData ad;
    ad.type = type;
    std::strncpy(ad.name, name ? name : "", sizeof(ad.name) - 1);
    ad.name[sizeof(ad.name) - 1] = 0;
    if (st.advert_loc_policy == companion::ADVERT_LOC_SHARE &&
        (st.lat_e6 != 0 || st.lon_e6 != 0)) {
      ad.has_loc = true;
      ad.lat = st.lat_e6;
      ad.lon = st.lon_e6;
    }
    // Shared finaliser: applies extension decorators, signs, routes and sends.
    return emitAdvert(*g_dispatcher, g_kernel, g_identity, g_rtc.now(), ad, flood);
  }
  uint32_t seq_ = 0;
};

static HeltecMeshSender g_sender;

// findCompanion locates the CompanionModule the generated composition
// registered, so we can attach the board's transport/display to it.
static CompanionModule* findCompanion() {
  for (int i = 0; i < g_kernel.moduleCount(); ++i) {
    Module* m = g_kernel.module(i);
    if (strcmp(m->name(), "companion") == 0) return static_cast<CompanionModule*>(m);
  }
  return nullptr;
}

#if COREFW_HAS_REPEATER
static RepeaterModule* findRepeater() {
  for (int i = 0; i < g_kernel.moduleCount(); ++i) {
    Module* m = g_kernel.module(i);
    if (strcmp(m->name(), "repeater") == 0) return static_cast<RepeaterModule*>(m);
  }
  return nullptr;
}
#endif

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

// The Heltec V3 has a single user button (PRG, GPIO0, active-low). One button
// drives the MeshCore-style companion pages: a short press advances to the next
// page, a long press (>=500 ms held) goes back. Debounced on press and release.
struct UserButton {
  uint8_t pin;
  bool down = false;         // currently pressed (post-debounce)
  uint32_t pressed_at = 0;   // millis() when the press began
  uint32_t last_change = 0;  // debounce timestamp

  void begin() {
    pinMode(pin, INPUT_PULLUP);
    down = (digitalRead(pin) == LOW);
    last_change = millis();
  }

  // Returns +1 on a short-press release, -1 on a long-press release, else 0.
  int poll() {
    bool raw = (digitalRead(pin) == LOW);
    uint32_t t = millis();
    if (raw == down || t - last_change < 40) return 0;  // no change / bounce
    last_change = t;
    down = raw;
    if (down) {  // just pressed
      pressed_at = t;
      return 0;
    }
    return (t - pressed_at >= 500) ? -1 : +1;  // released: long vs short
  }
};

#if defined(PIN_USER_BTN)
static UserButton g_btn{PIN_USER_BTN};
#endif

void setup() {
  // 1. Bring up the board (buses, radio, power rails) via the composition root.
  corefw_compose(g_kernel);
  if (g_kernel.board()) g_kernel.board()->begin();
  g_companion = findCompanion();
#if COREFW_HAS_REPEATER
  g_repeater = findRepeater();
#endif

  // Bring up the display early so startup stages are visible on the OLED.
  bool display_ok = g_display.begin();
  if (display_ok) {
    showStage("corefw display",
              g_kernel.board() ? g_kernel.board()->boardName() : "corefw");
    delay(500);
  }

  // 2. Mount the internal LittleFS (never formats an existing volume) and load
  //    identity, prefs, contacts and channels in MeshCore's on-flash formats. A
  //    fresh device generates a new identity here, seeded from the hardware RNG.
  showStage("startup", "storage");
  g_fs.begin();
  if (g_companion) {
    applyProfileRadioDefaults(g_companion->state());
    uint8_t seed[proto::SEED_SIZE];
    for (size_t i = 0; i < sizeof(seed); i++) seed[i] = uint8_t(esp_random());
    g_store.loadAll(g_companion->state(), seed);
    g_companion->state().active_ble_pin = activeBlePin(g_companion->state(), display_ok);
    g_identity = g_companion->state().self;
  }

  // 3. Build the radio scheduler around the board's configured radio.
  showStage("startup", "radio");
  RadioDriver* radio = g_kernel.board() ? g_kernel.board()->radio() : nullptr;
  RadioConfig cfg = g_companion ? makeRadioConfig(g_companion->state()) : RadioConfig{};
  static Dispatcher dispatcher(radio, &g_clock, g_identity.pub_key, cfg);
  g_dispatcher = &dispatcher;
  if (g_companion) {
    dispatcher.setAllowFloodForward(g_companion->state().client_repeat != 0);
  }
  if (radio) {
    g_radio_ok = radio->begin(cfg);
    if (!g_radio_ok) showStage("radio init", "failed");
  }
  // Deliver received packets to the companion, which decrypts those addressed
  // to this node and surfaces messages/adverts to the app.
  if (g_companion) {
    dispatcher.subscribe(g_companion);
    // Raw-RX diagnostics: stream every received frame to the app's packet log
    // (LOG_RX_DATA) through the companion's non-blocking transport queue.
    dispatcher.setRawRxObserver(g_companion);
    // Surface completed TRACE round-trips to the app (Trace Path view).
    dispatcher.setTraceObserver(g_companion);
  }

  // 4. Companion transport: USB-serial (simplest, no BLE stack) or NimBLE.
  if (g_companion && g_companion->transportKind() == CompanionTransportKind::USB) {
    showStage("startup", "usb serial");
    g_usb.begin(115200);
    g_transport = &g_usb;
  } else {
    showStage("startup", "ble");
    char ble_name[32];
    companion::formatBleDeviceName(
        ble_name, sizeof(ble_name),
        g_companion ? g_companion->state().node_name : "corefw");
    g_ble.begin(ble_name, g_companion ? g_companion->state().active_ble_pin : BLE_PIN_CODE);
    g_transport = &g_ble;
  }

  // 5. Attach everything to the companion module and start the kernel.
  if (g_companion) {
    CompanionModule* comp = g_companion;
    comp->attachClock(&g_clock);
    comp->attachHost(&g_host);
    comp->attachSender(&g_sender);
    comp->attachTransport(g_transport);
    comp->attachDisplay(kHasDisplay ? &g_display : nullptr);
  }

  static struct : PowerCoordinator {
    void requireRadioUntil(uint64_t) override {}
    void scheduleWake(uint64_t) override {}
    void preventDeepSleep(const char*) override {}
    void releaseDeepSleep(const char*) override {}
  } power;
  showStage("startup", "kernel");
  g_kernel.begin(dispatcher, power);
#if defined(PIN_USER_BTN)
  g_btn.begin();
#endif
  showStage("startup", "loop");
}

void loop() {
  const uint32_t now = g_clock.millis();
  if (g_dispatcher) g_dispatcher->loop();

  // Reflect transport connection state as kernel events, then pump the companion.
  static bool wasConnected = false;
  if (g_companion) {
#if defined(PIN_USER_BTN)
    int b = g_btn.poll();
    if (b != 0) g_companion->onButton(b);  // wakes the display, then pages
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

#if COREFW_HAS_REPEATER
  if (g_repeater && g_companion && g_radio_ok) {
    static bool boot_advert_sent = false;
    static uint32_t last_advert_ms = 0;
    const uint32_t interval_ms = g_repeater->advertIntervalSeconds() * 1000u;
    if (!boot_advert_sent && now >= 16000) {
      g_sender.sendRepeaterAdvert(/*flood=*/false);
      last_advert_ms = now;
      boot_advert_sent = true;
    } else if (boot_advert_sent && interval_ms != 0 && now - last_advert_ms >= interval_ms) {
      g_sender.sendRepeaterAdvert(/*flood=*/false);
      last_advert_ms = now;
    }
  }
#endif

#if defined(COREFW_ESP32_POWERSAVE)
  // Power: opt-in MCU light-sleep for ESP32 builds (default OFF). Unlike the
  // nRF52 WFE idle, esp_light_sleep_start() powers down the RF/BLE controller,
  // so it is ONLY safe when no companion link is live: a connected BLE/USB
  // session — or an open USB-CDC transport at all — would drop. We therefore
  // sleep only while disconnected, the OLED is blanked, and nothing is pending,
  // and only after a boot grace period so pairing/first-advert isn't disrupted.
  // Wakes early on a LoRa packet (DIO1) and at worst once a second to keep BLE
  // advertising alive. This mirrors MeshCore's opt-in, repeater-oriented sleep.
  {
    const bool link_live =
        (g_companion && g_companion->transportKind() == CompanionTransportKind::USB) ||
        (g_transport && g_transport->connected());
    const bool display_lit = kHasDisplay && g_display.isOn();
    const bool pending = (g_dispatcher && g_dispatcher->queueDepth() > 0) ||
                         (g_companion && g_companion->hasPendingWork());
    if (now >= 16000 && !link_live && !display_lit && !pending && g_kernel.board()) {
      g_kernel.board()->lightSleep(1000);
    }
  }
#endif
}

#endif  // COREFW_TARGET && ESP32_PLATFORM
