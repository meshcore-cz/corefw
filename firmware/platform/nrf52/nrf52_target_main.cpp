// nRF52 target entrypoint — shared by every nRF52840 corefw board.
//
// TARGET-ONLY reference wiring (COREFW_TARGET && NRF52_PLATFORM). This is the one
// concrete Arduino setup()/loop() for the nRF52 family (Wio Tracker L1, SenseCAP
// Solar, …): SX1262 radio + BLE/USB transport + optional OLED + optional buzzer.
// Board differences arrive as -D defines and through the Board object the
// generated composition root registered — this file constructs no board-specific
// peripheral by name. A build compiles exactly one platform main (this file for
// nRF52, esp32_target_main.cpp for ESP32), so there is never a duplicate
// setup()/loop() even with several nRF52 boards in the tree.
//
// The presence of a panel is compile-time: when the board manifest declares a
// display, DISPLAY_CLASS / DISPLAY_HEADER are defined and used; otherwise a
// NullDisplay is instantiated (headless repeater). A buzzer is wired only when
// the board defines PIN_BUZZER.
//
// It cannot be built by the host test suite (it needs the Adafruit nRF52 core,
// RadioLib and Bluefruit); the portable logic it drives is covered by the host
// tests under firmware/kernel/*.
#include <corefw/Kernel.h>

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM)

#include <Arduino.h>

// Companion protocol infrastructure lives in the kernel tree and is always
// available; the platform host/sender/receiver below are built on it. The
// CompanionModule *role class* is a self-encapsulated component, copied in and
// included only when the profile selected it (COREFW_HAS_COMPANION).
#include <corefw/companion/Commands.h>
#include <corefw/companion/Receiver.h>
#include <corefw/companion/State.h>
#include <corefw/companion/Storage.h>
#include <corefw/companion/Transport.h>
#if COREFW_HAS_COMPANION
#include <CompanionModule.h>
#endif
#if COREFW_HAS_REPEATER
#include <RepeaterModule.h>  // self-encapsulated component; copied in when selected
#endif
#include <corefw/protocol/Advert.h>
#include <corefw/runtime/Clock.h>
#include <corefw/runtime/Dispatcher.h>
#include <platform/shared/PlatformTx.h>

#include "NRF52BLETransport.h"
#include "NRF52Diag.h"
#include "NRF52FileStore.h"
#include "NRF52Gps.h"
#include "NRF52USBSerialTransport.h"

// Display: a real panel when the board manifest declares one (DISPLAY_CLASS /
// DISPLAY_HEADER emitted by codegen), otherwise a no-op NullDisplay so headless
// boards (e.g. the SenseCAP Solar repeater) drive the same code with no panel.
#if defined(DISPLAY_CLASS)
#include DISPLAY_HEADER
namespace { using BoardDisplay = corefw::ui::DISPLAY_CLASS; constexpr bool kHasDisplay = true; }
#else
#include <corefw/ui/NullDisplay.h>
namespace { using BoardDisplay = corefw::ui::NullDisplay; constexpr bool kHasDisplay = false; }
#endif

// A piezo buzzer is wired only on boards that define PIN_BUZZER.
#if defined(PIN_BUZZER)
// Lives outside the kernel include root; the generated build adds -I firmware.
#include <drivers/buzzer/ArduinoBuzzer.h>
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
static board::NRF52BLETransport g_ble;
static board::NRF52USBSerialTransport g_usb;
static BoardDisplay g_display;
#if defined(PIN_BUZZER)
static ui::ArduinoBuzzer g_buzzer(PIN_BUZZER);
#endif
static Dispatcher* g_dispatcher = nullptr;
static Kernel g_kernel;
static proto::LocalIdentity g_identity;
static board::NRF52FileStore g_fs;
static board::NRF52Gps g_gps;
static board::Watchdog g_wdt;
static board::SerialDebug g_dbg;

// Fault record lives in .noinit so its contents survive the reset that follows a
// hardfault; the boot banner reports the captured PC on the next start.
namespace corefw::board {
__attribute__((section(".noinit"))) FaultRecord g_fault_record;
}  // namespace corefw::board

// Cortex-M hardfault trampoline: pass the exception stack frame (MSP or PSP) to
// the capture routine, which records the faulting PC and reboots.
extern "C" void corefw_hardfault_capture(uint32_t* sp) {
  board::g_fault_record.magic = board::kFaultMagic;
  board::g_fault_record.pc = sp[6];   // stacked PC
  board::g_fault_record.lr = sp[5];   // stacked LR
  board::g_fault_record.cfsr = SCB->CFSR;
  NVIC_SystemReset();
}
extern "C" __attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile(
      " tst lr, #4                          \n"
      " ite eq                              \n"
      " mrseq r0, msp                       \n"
      " mrsne r0, psp                       \n"
      " ldr r1, =corefw_hardfault_capture   \n"
      " bx r1                               \n");
}
static companion::PersistentStore g_store(g_fs);
#if COREFW_HAS_COMPANION
static CompanionModule* g_companion = nullptr;  // resolved in setup()
#endif
// The active companion state (identity, radio, prefs). The platform host/sender
// read through this pointer, so they never need the CompanionModule type; it is
// set in setup() when a companion is present and stays null otherwise.
static companion::CompanionState* g_state = nullptr;
#if COREFW_HAS_REPEATER
static RepeaterModule* g_repeater = nullptr;    // resolved in setup() when selected
#endif
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
  void savePrefs() override { if (g_state) g_store.savePrefs(*g_state); }
  void saveContacts() override { if (g_state) g_store.saveContacts(*g_state); }
  void saveChannels() override { if (g_state) g_store.saveChannels(*g_state); }
  void applyRadioParams() override {
    if (!g_state || !g_dispatcher) return;
    g_dispatcher->configureRadio(makeRadioConfig(*g_state));
    g_dispatcher->setAllowFloodForward(g_state->client_repeat != 0);
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
  bool gpsEnabled() const override {
    return g_state && g_state->gps_enabled != 0 && g_gps.started();
  }
  bool gpsHasFix() const override { return g_gps.hasFix(); }
  uint8_t gpsSatellites() const override { return g_gps.satellites(); }
  int advertPath(const uint8_t* pub_key, uint32_t& recv_ts, uint8_t* path) override {
#if COREFW_HAS_COMPANION
    if (g_companion) return g_companion->advertPath(pub_key, recv_ts, path);
#else
    (void)pub_key; (void)recv_ts; (void)path;
#endif
    return -1;
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

static WioCompanionHost g_host;


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
                      g_state ? g_state->node_name : "corefw", flood);
  }
#if COREFW_HAS_REPEATER
  // Repeater self-advert (ADV_TYPE_REPEATER) so peers learn this is a relay.
  bool sendRepeaterAdvert(bool flood) {
    return sendAdvert(proto::ADV_TYPE_REPEATER,
                      g_repeater ? g_repeater->advertName() : "repeater", flood);
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
  uint32_t random32() override { rng_ = rng_ * 1664525u + 1013904223u; return rng_; }

 private:
  // Build and flood an advert of the given type/name. Location is attached only
  // when a companion is present, opted in, and we have a fix — so a pure
  // (companion-less) repeater still advertises its presence.
  bool sendAdvert(uint8_t type, const char* name, bool flood) {
    if (!g_dispatcher) return false;
    proto::AdvertData ad;
    ad.type = type;
    std::strncpy(ad.name, name ? name : "", sizeof(ad.name) - 1);
    ad.name[sizeof(ad.name) - 1] = 0;
    if (g_state) {
      const companion::CompanionState& st = *g_state;
      if (st.advert_loc_policy == companion::ADVERT_LOC_SHARE &&
          (st.lat_e6 != 0 || st.lon_e6 != 0)) {
        ad.has_loc = true;
        ad.lat = st.lat_e6;
        ad.lon = st.lon_e6;
      }
    }
    // Shared finaliser: applies extension decorators, signs, routes and sends.
    return emitAdvert(*g_dispatcher, g_kernel, g_identity, g_rtc.now(), ad, flood);
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

#if COREFW_HAS_COMPANION
// findCompanion locates the CompanionModule the generated composition
// registered, so we can attach the board's transport/display/buzzer to it.
static CompanionModule* findCompanion() {
  for (int i = 0; i < g_kernel.moduleCount(); ++i) {
    Module* m = g_kernel.module(i);
    if (strcmp(m->name(), "companion") == 0) return static_cast<CompanionModule*>(m);
  }
  return nullptr;
}
#endif

#if COREFW_HAS_REPEATER
// findRepeater locates the RepeaterModule when the profile selected the repeater
// role, so the loop can drive its periodic self-advert.
static RepeaterModule* findRepeater() {
  for (int i = 0; i < g_kernel.moduleCount(); ++i) {
    Module* m = g_kernel.module(i);
    if (strcmp(m->name(), "repeater") == 0) return static_cast<RepeaterModule*>(m);
  }
  return nullptr;
}
#endif

static void showStage(const char* line1, const char* line2 = "") {
  // Serial breadcrumb: the last stage printed before silence is where a freeze
  // happened (no-op on USB companion builds, where the CDC carries protocol).
  g_dbg.log("stage: %s %s", line1, line2);
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

// Render the previous boot's crash info to the OLED. This is the only diagnostic
// visible on a USB-companion build (where the serial console is the protocol
// channel), so it holds long enough to read or photograph.
static void showFreezeDiag() {
  if (!g_display.isOn()) return;
  char l[40];
  g_display.startFrame();
  g_display.setTextSize(1);
  g_display.setColor(ui::Display::LIGHT);
  g_display.setCursor(0, 0);
  g_display.print("FREEZE DIAG");
  g_display.setCursor(0, 12);
  g_display.print(board::ResetReason::text());
  g_display.setCursor(0, 24);
  std::snprintf(l, sizeof(l), "at: %s", board::crumbName(board::bootCrumb()));
  g_display.print(l);
  if (board::g_fault_record.magic == board::kFaultMagic) {
    g_display.setCursor(0, 36);
    std::snprintf(l, sizeof(l), "pc %08lx", (unsigned long)board::g_fault_record.pc);
    g_display.print(l);
    g_display.setCursor(0, 48);
    std::snprintf(l, sizeof(l), "cfsr %08lx", (unsigned long)board::g_fault_record.cfsr);
    g_display.print(l);
  }
  g_display.endFrame();
}

#if defined(COREFW_DIAG_DUMP)
// READ-ONLY identity/storage diagnostic. Mounts both filesystems (mount only —
// never formats, never writes) and prints every identity record it finds with
// its public key, so a clobbered/original identity can be located. Build with
//   PLATFORMIO_BUILD_FLAGS="-DCOREFW_DIAG_DUMP" pio run -t upload
// It loops forever at the end, so the normal (writing) setup never runs.
static void diagDumpIdentity(Adafruit_LittleFS& fs, const char* path) {
  if (!fs.exists(path)) { Serial.printf("  %s : (absent)\n", path); return; }
  Adafruit_LittleFS_Namespace::File f(fs);
  if (!f.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ)) {
    Serial.printf("  %s : (open failed)\n", path);
    return;
  }
  uint8_t rec[128] = {};
  size_t n = f.read(rec, sizeof(rec));
  f.close();
  Serial.printf("  %s : %u bytes\n", path, unsigned(n));
  if (n >= 96) {  // prv(64) || pub(32) [ || name(32) ]
    Serial.print("      pubkey = ");
    for (int i = 64; i < 96; i++) Serial.printf("%02x", rec[i]);
    Serial.println();
  }
  if (n > 96 && rec[96]) {
    Serial.print("      name   = ");
    for (int i = 96; i < int(n) && i < 128 && rec[i]; i++) Serial.write(rec[i]);
    Serial.println();
  }
}

static void diagListRoot(Adafruit_LittleFS& fs, const char* label) {
  Serial.printf("=== %s ===\n", label);
  Adafruit_LittleFS_Namespace::File dir(fs);
  if (dir.open("/", Adafruit_LittleFS_Namespace::FILE_O_READ)) {
    for (Adafruit_LittleFS_Namespace::File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      Serial.printf("  file: /%-14s %lu bytes\n", e.name(), (unsigned long)e.size());
      e.close();
    }
    dir.close();
  }
  diagDumpIdentity(fs, "/_main.id");
  diagDumpIdentity(fs, "/identity/_main.id");  // old corefw path
}

static void runIdentityDiag() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 5000) {}
  delay(800);
  Serial.println("\n\n### corefw identity diagnostic — READ-ONLY, writes nothing ###");
  InternalFS.begin();
  diagListRoot(InternalFS, "InternalFS");
#if defined(QSPIFLASH)
  static board::NRF52QSPIFileSystem qspi;
  if (qspi.begin()) {
    diagListRoot(qspi, "QSPI external flash");
  } else {
    Serial.println("=== QSPI external flash === (mount failed / no chip)");
  }
#endif
  Serial.println("### done. Power off safely — nothing was written. ###");
  for (;;) delay(1000);
}
#endif  // COREFW_DIAG_DUMP

void setup() {
#if defined(COREFW_DIAG_DUMP)
  runIdentityDiag();  // never returns — no writes happen past this point
#endif
  // Snapshot why we rebooted before anything else touches the register — a
  // "WATCHDOG" reason here is the signature of the freeze we are chasing.
  board::captureBoot();  // snapshot reset reason + last crumb before overwriting
  board::crumb(board::CRUMB_SETUP);

  // 1. Bring up the board (buses, radio, power rails) via the composition root.
  corefw_compose(g_kernel);
  if (g_kernel.board()) g_kernel.board()->begin();
  bool usb_companion = false;
#if COREFW_HAS_COMPANION
  g_companion = findCompanion();
  if (g_companion) g_state = &g_companion->state();
  usb_companion = g_companion && g_companion->transportKind() == CompanionTransportKind::USB;
#endif
#if COREFW_HAS_REPEATER
  g_repeater = findRepeater();
#endif

  // Serial debug console: usable whenever the USB CDC is not the companion
  // protocol transport (i.e. BLE builds), so it never corrupts framing.
  g_dbg.begin(/*enabled=*/!usb_companion);

  // Bring up the display early so every potentially blocking startup stage has
  // a visible breadcrumb on the OLED.
  bool display_ok = g_display.begin();
  if (display_ok) {
    if (board::crashedLastBoot()) {
      showFreezeDiag();  // reset reason + crumb + fault PC — hold to read it
      delay(6000);
    }
    showStage("corefw display",
              g_kernel.board() ? g_kernel.board()->boardName() : "corefw");
    delay(500);
  }
  board::g_fault_record.magic = 0;  // consumed by serial + OLED; clear for next boot

  // 2. Mount the existing filesystem (never formats) and load identity, prefs,
  //    contacts and channels in MeshCore's on-flash formats. A device reflashed
  //    from MeshCore keeps its identity (mesh address) and data. Only a fresh
  //    device generates a new identity here (seeded from the hardware RNG).
  // Storage mount. The QSPI guard lives inside NRF52FileStore (an InternalFS
  // marker file that survives any reset), so a crashing QSPI mount latches off
  // and can never boot-loop. The crumb records that we were here for the diag.
  showStage("startup", "storage");
  board::crumb(board::CRUMB_STORAGE_QSPI);
  g_fs.begin();
  board::crumb(board::CRUMB_SETUP);
  if (g_state) {
    applyProfileRadioDefaults(*g_state);
    uint8_t seed[proto::SEED_SIZE];
    for (size_t i = 0; i < sizeof(seed); i++) seed[i] = uint8_t(random(256));
    g_store.loadAll(*g_state, seed);
    g_identity = g_state->self;
  }

  // 3. Build the radio scheduler around the board's configured radio.
  showStage("startup", "radio");
  RadioDriver* radio = g_kernel.board() ? g_kernel.board()->radio() : nullptr;
  RadioConfig cfg = g_state ? makeRadioConfig(*g_state) : RadioConfig{};
  static Dispatcher dispatcher(radio, &g_clock, g_identity.pub_key, cfg);
  g_dispatcher = &dispatcher;
  if (g_state) {
    dispatcher.setAllowFloodForward(g_state->client_repeat != 0);
  }
  if (radio) {
    g_radio_ok = radio->begin(cfg);
    if (!g_radio_ok) showStage("radio init", "failed");
  }
  // Deliver received packets to the companion, which decrypts those addressed
  // to this node and surfaces messages/adverts to the app.
#if COREFW_HAS_COMPANION
  if (g_companion) {
    dispatcher.subscribe(g_companion);
    // Raw-RX diagnostics: stream every received frame to the app's packet log
    // (LOG_RX_DATA) through the companion's non-blocking transport queue.
    dispatcher.setRawRxObserver(g_companion);
    // Surface completed TRACE round-trips to the app (Trace Path view).
    dispatcher.setTraceObserver(g_companion);
  }
#endif

  // 4. Peripherals for the companion experience.
#if defined(PIN_BUZZER)
  showStage("startup", "buzzer");
  g_buzzer.begin();
#endif
  if (usb_companion) {
    showStage("startup", "usb serial");
    g_usb.begin(115200);
    g_transport = &g_usb;
  } else {
    showStage("startup", "ble");
    char ble_name[32];
    companion::formatBleDeviceName(
        ble_name, sizeof(ble_name),
        g_state ? g_state->node_name : "corefw");
    g_ble.begin(ble_name, BLE_PIN_CODE);
    g_transport = &g_ble;
  }

  // 5. Attach everything to the companion module and start the kernel.
#if COREFW_HAS_COMPANION
  if (g_companion) {
    CompanionModule* comp = g_companion;
    comp->attachClock(&g_clock);
    comp->attachHost(&g_host);
    comp->attachSender(&g_sender);
    comp->attachTransport(g_transport);
    comp->attachDisplay(kHasDisplay ? &g_display : nullptr);
#if defined(PIN_BUZZER)
    comp->attachBuzzer(&g_buzzer);
#endif
  }
#endif

#if defined(PIN_BUTTON4) && defined(PIN_BUTTON5) && defined(PIN_BUTTON6)
  g_btn_left.begin();
  g_btn_right.begin();
  g_btn_enter.begin();
#endif

  // 6. GPS: boards that report the gps capability (Wio Tracker L1, SenseCAP
  // Solar) carry an on-board NMEA receiver on Serial1. Track our own position so
  // it shows on-screen, feeds SELF_INFO, and can be shared in adverts. Enabled
  // by default since the hardware is present; the app can toggle it per session.
  if (g_state && g_kernel.board() && g_kernel.board()->capabilities().gps) {
    showStage("startup", "gps");
    g_state->gps_enabled = 1;
    g_gps.begin();
  }

  static struct : PowerCoordinator {
    void requireRadioUntil(uint64_t) override {}
    void scheduleWake(uint64_t) override {}
    void preventDeepSleep(const char*) override {}
    void releaseDeepSleep(const char*) override {}
  } power;
  showStage("startup", "kernel");
  g_kernel.begin(dispatcher, power);
#if COREFW_HAS_COMPANION
  if (g_companion) g_companion->playStartupMelody();
#endif
  showStage("startup", "loop");

  // Arm the watchdog last, so a hang anywhere in the steady-state loop (not the
  // one-time bring-up) trips it. 8s is comfortably longer than any legitimate
  // loop iteration, including BLE pairing.
  g_wdt.begin(8000);
}

// Fold GPS updates into device state: refresh our position, seed the RTC from
// satellite time when we have no other clock, and re-advertise on the configured
// interval so peers (and the map) see a fresh location.
static void serviceGps(uint32_t now_ms) {
  if (!g_state || !g_gps.started()) return;
  g_gps.loop();
  if (!g_gps.hasFix()) return;

  companion::CompanionState& st = *g_state;
  st.lat_e6 = g_gps.latE6();
  st.lon_e6 = g_gps.lonE6();

  // The Wio has no battery-backed RTC; adopt GPS UTC once if unset.
  uint32_t gps_time = g_gps.unixTime();
  if (gps_time != 0 && !g_rtc.isSet()) g_rtc.set(gps_time);

  // Periodic self-advert (gps_interval seconds, 0 = disabled).
  static uint32_t last_advert_ms = 0;
  uint32_t interval_ms = st.gps_interval * 1000u;
  if (interval_ms != 0 && (last_advert_ms == 0 || now_ms - last_advert_ms >= interval_ms)) {
    g_sender.sendSelfAdvert(/*flood=*/true);
    last_advert_ms = now_ms;
  }
}

void loop() {
  const uint32_t now = g_clock.millis();
  g_wdt.feed();
  g_dbg.heartbeat(now);
  board::crumb(board::CRUMB_LOOP_RX);
  if (g_dispatcher) g_dispatcher->loop();
  board::crumb(board::CRUMB_LOOP_GPS);
  serviceGps(now);
  board::crumb(board::CRUMB_LOOP_TICK);

  // Reflect transport connection state as kernel events, then pump the companion.
  static bool wasConnected = false;
#if COREFW_HAS_COMPANION
  if (g_companion) {
#if defined(PIN_BUTTON4) && defined(PIN_BUTTON5) && defined(PIN_BUTTON6)
    if (g_btn_left.pressed()) g_companion->onButton(-1);
    if (g_btn_right.pressed()) g_companion->onButton(+1);
    if (g_btn_enter.pressed()) g_companion->onButtonEnter();
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
#else
  (void)wasConnected;
#endif

#if COREFW_HAS_REPEATER
  // Repeater self-advert: one zero-hop advert shortly after boot (so neighbours
  // learn the relay) then periodically per the module's configured interval.
  if (g_repeater && g_radio_ok) {
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
  // Power: when there's nothing left to make progress on this pass, hand the
  // core to the SoftDevice until the next interrupt (radio DIO, BLE, GPS UART,
  // FreeRTOS tick). This is a light idle — the BLE link and radio receiver stay
  // live and scheduled retransmits still fire within a tick — but it collapses
  // the busy-loop's idle current. Safe on every nRF52 build, connected or not.
  bool pending = (g_dispatcher && g_dispatcher->queueDepth() > 0);
#if COREFW_HAS_COMPANION
  pending = pending || (g_companion && g_companion->hasPendingWork());
#endif
  if (!pending && g_kernel.board()) g_kernel.board()->lightSleep(0);

  board::crumb(board::CRUMB_LOOP_IDLE);
}

#endif  // COREFW_TARGET && NRF52_PLATFORM
