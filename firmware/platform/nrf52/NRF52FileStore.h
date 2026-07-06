// NRF52FileStore — FileStore backed by the Adafruit nRF52 LittleFS volumes.
//
// TARGET-ONLY. The Wio Tracker L1 companion layout matches MeshCore:
//   InternalFS: /_main.id, /new_prefs
//   QSPI flash: /contacts3, /channels2, /adv_blobs
// If QSPI cannot be mounted we fall back to InternalFS for contacts/channels so
// the device still boots and remains recoverable.
#pragma once

#include <corefw/companion/Storage.h>
#include <corefw/companion/StorageCodec.h>

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM)

#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <cstring>

#if defined(QSPIFLASH) && defined(COREFW_ENABLE_QSPI_STORE)
#include "NRF52QSPIFileSystem.h"
#endif

namespace corefw::board {

class NRF52FileStore : public companion::FileStore {
 public:
  // Mounts InternalFS (always) and, if enabled, the external QSPI volume.
  //
  // Mounting QSPI can hardfault or brown out the chip on some units. A marker
  // file on InternalFS (which survives ANY reset, unlike RAM) latches the
  // attempt: it is written before the mount and removed only once the mount
  // returns without crashing. So if a mount ever crashes, the marker persists
  // and QSPI is skipped on every later boot — the device can never boot-loop.
  void begin(bool /*unused*/ = true) {
    InternalFS.begin();
#if defined(QSPIFLASH) && defined(COREFW_ENABLE_QSPI_STORE)
    static const char* kQspiGuard = "/qspi_bad";
    if (!InternalFS.exists(kQspiGuard)) {
      uint8_t one = 1;
      writeInternal(kQspiGuard, &one, 1);   // "attempting" — persists if we crash
      qspi_mounted_ = qspi_.begin();
      InternalFS.remove(kQspiGuard);         // reached here → mount didn't crash
    }
#endif
    migrateToMeshCoreLayout();
  }

  bool exists(const char* path) override { return fsFor(path).exists(path); }

  size_t read(const char* path, uint8_t* buf, size_t cap) override {
    Adafruit_LittleFS& fs = fsFor(path);
    Adafruit_LittleFS_Namespace::File f(fs);
    if (!f.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ)) return 0;
    size_t n = f.read(buf, cap);
    f.close();
    return n;
  }

  bool write(const char* path, const uint8_t* data, size_t len) override {
    Adafruit_LittleFS& fs = fsFor(path);
    fs.remove(path);  // truncate/replace (LittleFS append semantics)
    Adafruit_LittleFS_Namespace::File f(fs);
    if (!f.open(path, Adafruit_LittleFS_Namespace::FILE_O_WRITE)) return false;
    size_t n = f.write(data, len);
    f.close();
    return n == len;
  }

  bool remove(const char* path) override { return fsFor(path).remove(path); }

  uint32_t usedKb() override { return usedKb(fsFor(companion::CONTACTS_FILE)); }

  uint32_t totalKb() override {
#if defined(QSPIFLASH) && defined(COREFW_ENABLE_QSPI_STORE)
    if (qspi_mounted_) return qspi_.totalKb();
#endif
    return totalKb(InternalFS);
  }

 private:
  static bool samePath(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

  static bool secondaryPath(const char* path) {
    return samePath(path, companion::CONTACTS_FILE) || samePath(path, companion::CHANNELS_FILE) ||
           samePath(path, companion::ADV_BLOBS_FILE);
  }

  Adafruit_LittleFS& fsFor(const char* path) {
#if defined(QSPIFLASH) && defined(COREFW_ENABLE_QSPI_STORE)
    if (qspi_mounted_ && secondaryPath(path)) return qspi_;
#endif
    return InternalFS;
  }

  static bool copyFile(Adafruit_LittleFS& from, Adafruit_LittleFS& to, const char* path) {
    Adafruit_LittleFS_Namespace::File src(from);
    if (!src.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ)) return false;
    to.remove(path);
    Adafruit_LittleFS_Namespace::File dst(to);
    if (!dst.open(path, Adafruit_LittleFS_Namespace::FILE_O_WRITE)) {
      src.close();
      return false;
    }
    uint8_t buf[128];
    bool ok = true;
    while (true) {
      size_t n = src.read(buf, sizeof(buf));
      if (n == 0) break;
      if (dst.write(buf, n) != n) {
        ok = false;
        break;
      }
    }
    src.close();
    dst.close();
    return ok;
  }

  static void moveIfPresent(Adafruit_LittleFS& from, Adafruit_LittleFS& to, const char* path) {
    if (!from.exists(path)) return;
    if (copyFile(from, to, path)) from.remove(path);
  }

  static void moveIfMissing(Adafruit_LittleFS& from, Adafruit_LittleFS& to, const char* path) {
    if (!from.exists(path)) return;
    if (to.exists(path)) {
      from.remove(path);
      return;
    }
    moveIfPresent(from, to, path);
  }

  void migrateToMeshCoreLayout() {
    // Old corefw builds used /identity/_main.id. MeshCore nRF builds use /_main.id.
    if (!InternalFS.exists(companion::IDENTITY_FILE) && InternalFS.exists("/identity/_main.id")) {
      uint8_t rec[companion::IDENTITY_RECORD_SIZE];
      size_t n = readInternal("/identity/_main.id", rec, sizeof(rec));
      if (n > 0 && writeInternal(companion::IDENTITY_FILE, rec, n)) {
        InternalFS.remove("/identity/_main.id");
      }
    }

#if defined(QSPIFLASH) && defined(COREFW_ENABLE_QSPI_STORE)
    if (!qspi_mounted_) return;
    moveIfMissing(InternalFS, qspi_, companion::ADV_BLOBS_FILE);
    moveIfMissing(InternalFS, qspi_, companion::CONTACTS_FILE);
    moveIfMissing(InternalFS, qspi_, companion::CHANNELS_FILE);
    // Identity and prefs are authoritative on InternalFS (this matches stock
    // MeshCore: loadMainIdentity and "/new_prefs" both read the primary FS). Only
    // pull them off QSPI when InternalFS has none — NEVER overwrite an existing
    // InternalFS identity with a stale QSPI copy, or the node's keys and name get
    // clobbered. (moveIfPresent here previously did exactly that.)
    moveIfMissing(qspi_, InternalFS, companion::IDENTITY_FILE);
    moveIfMissing(qspi_, InternalFS, companion::PREFS_FILE);
#endif
  }

  static size_t readInternal(const char* path, uint8_t* buf, size_t cap) {
    Adafruit_LittleFS_Namespace::File f(InternalFS);
    if (!f.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ)) return 0;
    size_t n = f.read(buf, cap);
    f.close();
    return n;
  }

  static bool writeInternal(const char* path, const uint8_t* data, size_t len) {
    InternalFS.remove(path);
    Adafruit_LittleFS_Namespace::File f(InternalFS);
    if (!f.open(path, Adafruit_LittleFS_Namespace::FILE_O_WRITE)) return false;
    size_t n = f.write(data, len);
    f.close();
    return n == len;
  }

  static int countBlock(void* p, lfs_block_t block) {
    (void)block;
    *static_cast<uint32_t*>(p) += 1;
    return 0;
  }

  static uint32_t usedKb(Adafruit_LittleFS& fs) {
    uint32_t blocks = 0;
    lfs_traverse(fs._getFS(), countBlock, &blocks);
    const lfs_config* cfg = fs._getFS()->cfg;
    return (blocks * cfg->block_size) / 1024;
  }

  static uint32_t totalKb(Adafruit_LittleFS& fs) {
    const lfs_config* cfg = fs._getFS()->cfg;
    return (cfg->block_count * cfg->block_size) / 1024;
  }

#if defined(QSPIFLASH) && defined(COREFW_ENABLE_QSPI_STORE)
  NRF52QSPIFileSystem qspi_;
  bool qspi_mounted_ = false;
#endif
};

}  // namespace corefw::board

#endif  // COREFW_TARGET && NRF52_PLATFORM
