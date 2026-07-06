// QSPI LittleFS adapter for Adafruit nRF52 boards.
//
// MeshCore's Wio Tracker L1 companion build stores contacts/channels on the
// external P25Q16H QSPI flash. This mounts that same LittleFS volume without
// formatting it.
#pragma once

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM) && defined(QSPIFLASH)

#include <Adafruit_LittleFS.h>
#include <Adafruit_SPIFlash.h>

namespace corefw::board {

class NRF52QSPIFileSystem : public Adafruit_LittleFS {
 public:
  NRF52QSPIFileSystem() : Adafruit_LittleFS(&config_) {}

  bool begin() {
    static const SPIFlash_Device_t devices[] = {EXTERNAL_FLASH_DEVICES};
    if (!flash_.begin(devices, sizeof(devices) / sizeof(devices[0]))) return false;
    config_.block_count = flash_.size() / kBlockSize;
    if (Adafruit_LittleFS::begin(&config_)) return true;
    if (!isBlank()) return false;
    if (!format()) return false;
    return Adafruit_LittleFS::begin(&config_);
  }

  uint32_t totalKb() const { return (config_.block_count * kBlockSize) / 1024; }

 private:
  static constexpr uint32_t kBlockSize = 4096;
  static constexpr uint32_t kProgSize = 256;

  static uint32_t addr(lfs_block_t block, lfs_off_t off = 0) {
    return uint32_t(block) * kBlockSize + off;
  }

  static int readCb(const struct lfs_config* c, lfs_block_t block, lfs_off_t off,
                    void* buffer, lfs_size_t size) {
    (void)c;
    return flash_.readBuffer(addr(block, off), static_cast<uint8_t*>(buffer), size) == size
               ? 0
               : LFS_ERR_IO;
  }

  static int progCb(const struct lfs_config* c, lfs_block_t block, lfs_off_t off,
                    const void* buffer, lfs_size_t size) {
    (void)c;
    return flash_.writeBuffer(addr(block, off), static_cast<const uint8_t*>(buffer), size) == size
               ? 0
               : LFS_ERR_IO;
  }

  static int eraseCb(const struct lfs_config* c, lfs_block_t block) {
    (void)c;
    return flash_.eraseSector(block) ? 0 : LFS_ERR_IO;
  }

  static int syncCb(const struct lfs_config* c) {
    (void)c;
    flash_.waitUntilReady();
    return 0;
  }

  static bool isBlank() {
    uint8_t buf[64];
    for (uint32_t off = 0; off < kBlockSize && off < flash_.size(); off += sizeof(buf)) {
      if (flash_.readBuffer(off, buf, sizeof(buf)) != sizeof(buf)) return false;
      for (uint8_t b : buf) {
        if (b != 0xFF) return false;
      }
    }
    return true;
  }

  static Adafruit_FlashTransport_QSPI transport_;
  static Adafruit_SPIFlash flash_;
  static struct lfs_config config_;
};

inline Adafruit_FlashTransport_QSPI NRF52QSPIFileSystem::transport_;
inline Adafruit_SPIFlash NRF52QSPIFileSystem::flash_(&NRF52QSPIFileSystem::transport_, false);
inline struct lfs_config NRF52QSPIFileSystem::config_ = {
    .context = nullptr,
    .read = NRF52QSPIFileSystem::readCb,
    .prog = NRF52QSPIFileSystem::progCb,
    .erase = NRF52QSPIFileSystem::eraseCb,
    .sync = NRF52QSPIFileSystem::syncCb,
    .read_size = NRF52QSPIFileSystem::kProgSize,
    .prog_size = NRF52QSPIFileSystem::kProgSize,
    .block_size = NRF52QSPIFileSystem::kBlockSize,
    .block_count = 0,
    .lookahead = 128,
    .read_buffer = nullptr,
    .prog_buffer = nullptr,
    .lookahead_buffer = nullptr,
    .file_buffer = nullptr,
};

}  // namespace corefw::board

#endif  // COREFW_TARGET && NRF52_PLATFORM && QSPIFLASH
