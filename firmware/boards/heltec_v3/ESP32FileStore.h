// ESP32FileStore — FileStore backed by the ESP32 SPIFFS data partition.
//
// TARGET-ONLY. The Heltec V3 has no external QSPI flash, so identity, prefs,
// contacts and channels all live in one internal volume — the simple, reliable
// case (no dual-FS, no QSPI mount to brown out).
//
// Uses SPIFFS (not LittleFS) *deliberately*: MeshCore's ESP32 companion stores
// its data on SPIFFS (companion_radio/main.cpp `SPIFFS.begin(true)`), and both
// firmwares use the identical default_8MB.csv partition table (data partition
// `spiffs` @ 0x670000). Mounting the same partition with the same filesystem —
// plus the byte-compatible MeshCore record layouts (StorageCodec.h) — is what
// lets a Heltec reflashed from stock MeshCore keep its identity and contacts.
// A LittleFS mount here would fail on MeshCore's SPIFFS volume and reformat it,
// destroying the node identity. See docs/FLASH-SAFETY.md.
#pragma once

#include <corefw/companion/Storage.h>

#if defined(COREFW_TARGET)

#include <SPIFFS.h>

namespace corefw::board {

class ESP32FileStore : public companion::FileStore {
 public:
  // Mounts SPIFFS, formatting only a brand-new/blank partition (never an
  // existing MeshCore/corefw volume — that would wipe identity + contacts).
  // Matches MeshCore's `SPIFFS.begin(true)`.
  bool begin() {
    mounted_ = SPIFFS.begin(/*formatOnFail=*/true);
    return mounted_;
  }

  bool exists(const char* path) override { return mounted_ && SPIFFS.exists(path); }

  size_t read(const char* path, uint8_t* buf, size_t cap) override {
    if (!mounted_) return 0;
    File f = SPIFFS.open(path, FILE_READ);
    if (!f) return 0;
    size_t n = f.read(buf, cap);
    f.close();
    return n;
  }

  bool write(const char* path, const uint8_t* data, size_t len) override {
    if (!mounted_) return false;
    File f = SPIFFS.open(path, FILE_WRITE);  // truncates/creates
    if (!f) return false;
    size_t n = f.write(data, len);
    f.close();
    return n == len;
  }

  bool remove(const char* path) override { return mounted_ && SPIFFS.remove(path); }

  uint32_t usedKb() override { return mounted_ ? SPIFFS.usedBytes() / 1024 : 0; }
  uint32_t totalKb() override { return mounted_ ? SPIFFS.totalBytes() / 1024 : 0; }

 private:
  bool mounted_ = false;
};

}  // namespace corefw::board

#endif  // COREFW_TARGET
