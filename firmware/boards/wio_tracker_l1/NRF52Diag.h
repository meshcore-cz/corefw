// NRF52Diag — freeze diagnostics for the Wio Tracker L1 companion.
//
// Three pieces that together make a "constant freezing" bug observable and
// self-recovering:
//   * Watchdog     — an nRF52 WDT that resets the chip if the main loop stops
//                    feeding it, so a hang becomes an automatic reboot instead
//                    of a dead device.
//   * Reset reason — on boot we read POWER->RESETREAS; the DOG bit proves the
//                    last boot ended in a watchdog-detected freeze. Surfaced on
//                    the OLED and over serial.
//   * SerialDebug  — a heartbeat + stage trace over the USB CDC. On BLE builds
//                    the CDC is otherwise unused, so this is a free console:
//                    the last stage printed before silence is where it froze.
//
// TARGET-ONLY. Debug output is suppressed when the USB CDC is the companion
// protocol transport (a USB companion build), so it never corrupts framing.
#pragma once

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM)

#include <Arduino.h>
#include <nrf.h>

#include <cstdarg>
#include <cstdio>

namespace corefw::board {

// Breadcrumbs — a single byte written to POWER->GPREGRET2, which the hardware
// retains across watchdog/soft resets (but not power-off). Set one cheaply
// before each risky step; after a freeze-and-reset the last one written is
// readable on the next boot, pinning where the firmware died. Bit 7 marks the
// value valid so a genuine power-on (register 0) is not mistaken for crumb 0.
enum Crumb : uint8_t {
  CRUMB_NONE = 0,
  CRUMB_SETUP,
  CRUMB_STORAGE_QSPI,  // mounting the QSPI volume (can hardfault on a bad chip)
  CRUMB_LOOP_RX,       // inside Dispatcher::loop (radio RX + decode + deliver)
  CRUMB_LOOP_GPS,      // servicing the GPS
  CRUMB_LOOP_TICK,     // companion tick (transport pump, UI render)
  CRUMB_LOOP_IDLE,     // end of a clean loop iteration
  CRUMB_RADIO_READ,    // SX1262 readReceived
  CRUMB_COUNT,
};

inline void crumb(uint8_t id) { NRF_POWER->GPREGRET2 = uint8_t(id | 0x80); }
inline uint8_t lastCrumb() {
  uint8_t v = NRF_POWER->GPREGRET2;
  return (v & 0x80) ? uint8_t(v & 0x7F) : 0;
}

// The crumb from *before* this boot, snapshotted by captureBoot() before any new
// crumb overwrites the retention register. Read this, not lastCrumb(), to see
// where a freeze happened.
inline uint8_t g_boot_crumb = 0;
inline uint8_t bootCrumb() { return g_boot_crumb; }
inline const char* crumbName(uint8_t id) {
  switch (id) {
    case CRUMB_SETUP: return "setup";
    case CRUMB_STORAGE_QSPI: return "qspi mount";
    case CRUMB_LOOP_RX: return "radio RX/decode";
    case CRUMB_LOOP_GPS: return "gps";
    case CRUMB_LOOP_TICK: return "companion tick";
    case CRUMB_LOOP_IDLE: return "idle";
    case CRUMB_RADIO_READ: return "radio read";
    default: return "none";
  }
}

// Fault capture. A hardfault stores the faulting PC and a magic in a RAM region
// the C runtime does not clear (.noinit), so the address survives the reset and
// can be looked up against the .elf on the next boot.
struct FaultRecord {
  uint32_t magic;
  uint32_t pc;
  uint32_t lr;
  uint32_t cfsr;
};
extern FaultRecord g_fault_record;
inline constexpr uint32_t kFaultMagic = 0xFA017EC0;

// Human-readable decode of POWER->RESETREAS. The register is sticky: bits stay
// set across resets until cleared, so we snapshot and clear it once at boot.
class ResetReason {
 public:
  static void capture() {
    reas_ = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = 0xFFFFFFFF;  // clear all sticky bits for next time
  }
  static uint32_t raw() { return reas_; }
  static bool wasWatchdog() { return (reas_ & POWER_RESETREAS_DOG_Msk) != 0; }
  static const char* text() {
    if (reas_ == 0) return "power-on";
    if (reas_ & POWER_RESETREAS_DOG_Msk) return "WATCHDOG (froze)";
    if (reas_ & POWER_RESETREAS_RESETPIN_Msk) return "reset pin";
    if (reas_ & POWER_RESETREAS_SREQ_Msk) return "soft reset";
    if (reas_ & POWER_RESETREAS_LOCKUP_Msk) return "cpu lockup";
    if (reas_ & POWER_RESETREAS_OFF_Msk) return "wake from off";
    return "other";
  }

 private:
  static inline uint32_t reas_ = 0;
};

// Snapshot everything the previous boot left behind, before we overwrite any of
// it. Must run first in setup(): the reset reason (sticky) and the last
// breadcrumb (retention register, clobbered by the next crumb()).
inline void captureBoot() {
  ResetReason::capture();
  g_boot_crumb = lastCrumb();
}

// True if the previous boot ended abnormally (watchdog freeze or hardfault).
inline bool crashedLastBoot() {
  return ResetReason::wasWatchdog() || g_fault_record.magic == kFaultMagic;
}

// A watchdog that reboots the board if it is not fed within `timeout`. Once
// started it cannot be stopped or reconfigured (nRF52 hardware rule), and it
// pauses while the debugger halts the CPU so it does not fire during debugging.
class Watchdog {
 public:
  void begin(uint32_t timeout_ms = 8000) {
    if (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) { running_ = true; return; }
    NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) |
                      (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
    NRF_WDT->CRV = uint32_t((uint64_t(timeout_ms) * 32768) / 1000);  // 32.768 kHz
    NRF_WDT->RREN = WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos;
    NRF_WDT->TASKS_START = 1;
    running_ = true;
  }
  void feed() {
    if (running_) NRF_WDT->RR[0] = WDT_RR_RR_Reload;
  }

 private:
  bool running_ = false;
};

// Lightweight serial console: a per-second heartbeat plus a "stage" marker so a
// freeze is pinned to the last thing the firmware announced.
class SerialDebug {
 public:
  // `runtime_enabled` gates the per-loop heartbeat/stage/log (BLE builds only —
  // on a USB-companion build the CDC carries protocol). The one-shot boot report
  // always prints: it runs before the transport starts pumping, so it cannot
  // interleave with protocol frames even on the USB build.
  void begin(bool runtime_enabled) {
    enabled_ = runtime_enabled;
    Serial.begin(115200);
    Serial.println("=== corefw wio companion boot ===");
    Serial.print("last reset: ");
    Serial.println(ResetReason::text());
    if (ResetReason::wasWatchdog()) {
      Serial.print("!! froze at: ");
      Serial.print(crumbName(bootCrumb()));
      Serial.print(" (crumb ");
      Serial.print(bootCrumb());
      Serial.println(")");
    }
    if (g_fault_record.magic == kFaultMagic) {
      char buf[80];
      std::snprintf(buf, sizeof(buf), "!! HARDFAULT pc=0x%08lx lr=0x%08lx cfsr=0x%08lx",
                    (unsigned long)g_fault_record.pc, (unsigned long)g_fault_record.lr,
                    (unsigned long)g_fault_record.cfsr);
      Serial.println(buf);
    }
  }

  void setStage(const char* stage) {
    stage_ = stage;
    log("stage: %s", stage);
  }

  // Call every loop; prints a heartbeat roughly once a second. `loops` lets the
  // heartbeat show throughput, so a stalled-but-alive loop is distinguishable
  // from a truly frozen one.
  void heartbeat(uint32_t now_ms) {
    if (!enabled_) return;
    loops_++;
    if (now_ms - last_beat_ms_ < 1000) return;
    log("hb t=%lus loops=%lu stage=%s freeHeap~ok", (unsigned long)(now_ms / 1000),
        (unsigned long)loops_, stage_ ? stage_ : "-");
    last_beat_ms_ = now_ms;
    loops_ = 0;
  }

  void log(const char* fmt, ...) {
    if (!enabled_) return;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.println(buf);
  }

  bool enabled() const { return enabled_; }

 private:
  bool enabled_ = false;
  const char* stage_ = nullptr;
  uint32_t last_beat_ms_ = 0;
  uint32_t loops_ = 0;
};

}  // namespace corefw::board

#endif  // COREFW_TARGET && NRF52_PLATFORM
