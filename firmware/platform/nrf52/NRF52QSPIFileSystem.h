// QSPI LittleFS adapter for the Wio Tracker L1's external P25Q16H flash.
//
// MeshCore's Wio companion stores contacts/channels on this chip. We mount the
// SAME LittleFS volume (Adafruit's bundled lfs, block 4096 / page 256) so a
// device reflashed from stock keeps its contact list.
//
// The flash is driven through the raw nRF QSPI peripheral (nrfx_qspi) in
// BLOCKING mode — the same low-level path MeshCore's CustomLFS_QSPIFlash uses.
// An earlier version went through Adafruit_SPIFlash and hardfaulted during
// lfs_mount on hardware; nrfx_qspi is the proven-working access layer.
#pragma once

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM) && defined(QSPIFLASH)

#include <Arduino.h>
#include <Adafruit_LittleFS.h>
#include <nrfx_qspi.h>

#include <cstring>

// Arduino pin number -> nRF port/pin, defined by the core.
extern "C" {
extern const uint32_t g_ADigitalPinMap[];
}

namespace corefw::board {

// LittleFS working buffers. word-aligned because nrfx_qspi requires 4-byte
// aligned RAM buffers; sizes match read/prog size and the lookahead, exactly as
// MeshCore's CustomLFS_QSPIFlash provides them.
alignas(4) inline uint8_t g_qspi_read_buf[256];
alignas(4) inline uint8_t g_qspi_prog_buf[256];
alignas(4) inline uint8_t g_qspi_lookahead_buf[32];

class NRF52QSPIFileSystem : public Adafruit_LittleFS {
 public:
  NRF52QSPIFileSystem() : Adafruit_LittleFS(&config_) {}

  bool begin() {
    if (mounted_) return true;

    // Build the config by hand (the NRFX_QSPI_DEFAULT_CONFIG macro references an
    // sdk_config IRQ-priority symbol the Adafruit build doesn't define). Values
    // mirror that macro plus MeshCore's conservative single-line settings.
    nrfx_qspi_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.pins.sck_pin = g_ADigitalPinMap[PIN_QSPI_SCK];
    cfg.pins.csn_pin = g_ADigitalPinMap[PIN_QSPI_CS];
    cfg.pins.io0_pin = g_ADigitalPinMap[PIN_QSPI_IO0];
    cfg.pins.io1_pin = g_ADigitalPinMap[PIN_QSPI_IO1];
    cfg.pins.io2_pin = g_ADigitalPinMap[PIN_QSPI_IO2];
    cfg.pins.io3_pin = g_ADigitalPinMap[PIN_QSPI_IO3];
    cfg.prot_if.readoc = NRF_QSPI_READOC_FASTREAD;
    cfg.prot_if.writeoc = NRF_QSPI_WRITEOC_PP;
    cfg.prot_if.addrmode = NRF_QSPI_ADDRMODE_24BIT;
    cfg.prot_if.dpmconfig = false;
    cfg.phy_if.sck_delay = 5;
    cfg.phy_if.dpmen = false;
    cfg.phy_if.spi_mode = NRF_QSPI_MODE_0;
    cfg.phy_if.sck_freq = NRF_QSPI_FREQ_DIV16;
    cfg.irq_priority = 7;

    // handler == NULL -> blocking mode: read/write/erase return once complete.
    nrfx_err_t err = nrfx_qspi_init(&cfg, nullptr, nullptr);
    if (err == NRFX_ERROR_INVALID_STATE) {  // already initialised — reclaim it
      nrfx_qspi_uninit();
      err = nrfx_qspi_init(&cfg, nullptr, nullptr);
    }
    if (err != NRFX_SUCCESS) return false;

    // Wake the flash from deep power-down and read its JEDEC id. MeshCore does
    // this (via detectChip) before any lfs access; skipping it means the very
    // first lfs_mount read can hit a sleeping chip and fault. The JEDEC read
    // also confirms the chip is actually responding before we mount.
    nrf_qspi_cinstr_conf_t rdpd = {};
    rdpd.opcode = 0xAB;  // Release from Deep Power-Down
    rdpd.length = NRF_QSPI_CINSTR_LEN_1B;
    rdpd.io2_level = true;
    rdpd.io3_level = true;
    nrfx_qspi_cinstr_xfer(&rdpd, nullptr, nullptr);
    delay(2);

    uint8_t jedec[3] = {0, 0, 0};
    nrf_qspi_cinstr_conf_t rdid = {};
    rdid.opcode = 0x9F;  // Read JEDEC id
    rdid.length = NRF_QSPI_CINSTR_LEN_4B;
    rdid.io2_level = true;
    rdid.io3_level = true;
    if (nrfx_qspi_cinstr_xfer(&rdid, nullptr, jedec) != NRFX_SUCCESS) return false;
    bool valid = !((jedec[0] == 0 && jedec[1] == 0 && jedec[2] == 0) ||
                   (jedec[0] == 0xFF && jedec[1] == 0xFF && jedec[2] == 0xFF));
    if (!valid) return false;  // no chip responding — don't attempt a mount

    config_.block_count = kTotalSize / kBlockSize;
    // Mount the existing volume. Never auto-format: that would erase the very
    // contacts we came here to read. A blank/fresh chip just fails to mount and
    // the caller falls back to InternalFS.
    mounted_ = Adafruit_LittleFS::begin(&config_);
    return mounted_;
  }

  uint32_t totalKb() const { return kTotalSize / 1024; }

 private:
  static constexpr uint32_t kBlockSize = 4096;  // 4 KB sector
  static constexpr uint32_t kPageSize = 256;    // 256 B page
  static constexpr uint32_t kTotalSize = 2u * 1024 * 1024;  // P25Q16H = 16 Mbit

  static int readCb(const struct lfs_config* c, lfs_block_t block, lfs_off_t off,
                    void* buffer, lfs_size_t size) {
    (void)c;
    uint32_t addr = uint32_t(block) * kBlockSize + off;
    return nrfx_qspi_read(buffer, size, addr) == NRFX_SUCCESS ? 0 : LFS_ERR_IO;
  }
  static int progCb(const struct lfs_config* c, lfs_block_t block, lfs_off_t off,
                    const void* buffer, lfs_size_t size) {
    (void)c;
    uint32_t addr = uint32_t(block) * kBlockSize + off;
    return nrfx_qspi_write(buffer, size, addr) == NRFX_SUCCESS ? 0 : LFS_ERR_IO;
  }
  static int eraseCb(const struct lfs_config* c, lfs_block_t block) {
    (void)c;
    uint32_t addr = uint32_t(block) * kBlockSize;
    return nrfx_qspi_erase(NRF_QSPI_ERASE_LEN_4KB, addr) == NRFX_SUCCESS ? 0 : LFS_ERR_IO;
  }
  static int syncCb(const struct lfs_config* c) { (void)c; return 0; }  // blocking: always synced

  bool mounted_ = false;
  static struct lfs_config config_;
};

inline struct lfs_config NRF52QSPIFileSystem::config_ = {
    .context = nullptr,
    .read = NRF52QSPIFileSystem::readCb,
    .prog = NRF52QSPIFileSystem::progCb,
    .erase = NRF52QSPIFileSystem::eraseCb,
    .sync = NRF52QSPIFileSystem::syncCb,
    .read_size = 256,
    .prog_size = 256,
    .block_size = 4096,
    .block_count = 0,
    .lookahead = 32,  // matches MeshCore's CustomLFS_QSPIFlash
    .read_buffer = g_qspi_read_buf,
    .prog_buffer = g_qspi_prog_buf,
    .lookahead_buffer = g_qspi_lookahead_buf,
    .file_buffer = nullptr,
};

}  // namespace corefw::board

#endif  // COREFW_TARGET && NRF52_PLATFORM && QSPIFLASH
