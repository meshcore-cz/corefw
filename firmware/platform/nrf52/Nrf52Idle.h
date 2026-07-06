// Nrf52Idle — event-driven MCU idle for nRF52 boards.
//
// Shared by every nRF52 board's Board::lightSleep(). Mirrors MeshCore's
// NRF52Board::sleep(): hand the core to the SoftDevice (or a raw WFE) until the
// next interrupt. The radio DIO, BLE, UART and the FreeRTOS tick are all wake
// sources, so the connection stays live and scheduled work still fires within a
// tick — this is a light idle, not deep sleep.
#pragma once

#if defined(COREFW_TARGET)

#include <Arduino.h>
#include <nrf_sdm.h>   // sd_softdevice_is_enabled
#include <nrf_soc.h>   // sd_app_evt_wait

namespace corefw {

inline void nrf52LightSleep() {
  // Clear FPU exception flags first (nRF52840 errata 87) — a pending FPU IRQ
  // would otherwise wake the core immediately and defeat the sleep.
#if defined(__FPU_USED) && (__FPU_USED == 1)
  __set_FPSCR(__get_FPSCR() & ~(0x0000009Ful));
  (void)__get_FPSCR();
  NVIC_ClearPendingIRQ(FPU_IRQn);
#endif

  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);
  if (sd_enabled) {
    // First call drains any pending SoftDevice events; the second sleeps until
    // the next event arrives.
    sd_app_evt_wait();
    sd_app_evt_wait();
  } else {
    __SEV();
    __WFE();
    __WFE();
  }
}

}  // namespace corefw

#endif  // COREFW_TARGET
