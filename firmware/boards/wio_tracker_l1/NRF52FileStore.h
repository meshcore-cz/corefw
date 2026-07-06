// NRF52FileStore — FileStore backed by the Adafruit nRF52 InternalFS (LittleFS).
//
// TARGET-ONLY. This is the same filesystem MeshCore uses on the nRF52840, so
// corefw reads and writes the identical files (/identity/_main.id, /new_prefs,
// /contacts3, /channels2). It NEVER formats: begin() only mounts. A device
// reflashed from MeshCore keeps its identity and contacts because the InternalFS
// pages live in the flash region the app image does not overwrite.
#pragma once

#include <corefw/companion/Storage.h>

#if defined(COREFW_TARGET) && defined(NRF52_PLATFORM)

#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

namespace corefw::board {

class NRF52FileStore : public companion::FileStore {
 public:
  // Mount the existing filesystem (does not format). Adafruit's InternalFS.begin
  // creates the FS only if none is present, preserving existing data.
  void begin() { InternalFS.begin(); ensureDir("/identity"); }

  bool exists(const char* path) override { return InternalFS.exists(path); }

  size_t read(const char* path, uint8_t* buf, size_t cap) override {
    Adafruit_LittleFS_Namespace::File f(InternalFS);
    if (!f.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ)) return 0;
    size_t n = f.read(buf, cap);
    f.close();
    return n;
  }

  bool write(const char* path, const uint8_t* data, size_t len) override {
    InternalFS.remove(path);  // truncate/replace (LittleFS append semantics)
    Adafruit_LittleFS_Namespace::File f(InternalFS);
    if (!f.open(path, Adafruit_LittleFS_Namespace::FILE_O_WRITE)) return false;
    size_t n = f.write(data, len);
    f.close();
    return n == len;
  }

  bool remove(const char* path) override { return InternalFS.remove(path); }

 private:
  static void ensureDir(const char* dir) {
    if (!InternalFS.exists(dir)) InternalFS.mkdir(dir);
  }
};

}  // namespace corefw::board

#endif  // COREFW_TARGET && NRF52_PLATFORM
