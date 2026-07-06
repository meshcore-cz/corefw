// PersistentStore — loads and saves the companion's identity, preferences,
// contacts and channels through a tiny FileStore backend, using the
// byte-compatible codecs. This is the portable half; a board supplies the
// FileStore (Adafruit InternalFS on nRF52, an in-memory map in host tests).
//
// Compatibility & safety: load() reads the existing MeshCore files if present
// and NEVER formats the filesystem. A device reflashed from MeshCore to corefw
// keeps its identity (hence its mesh address) and contacts. Only explicit
// CMD_FACTORY_RESET erases anything.
#pragma once

#include <corefw/companion/State.h>
#include <corefw/companion/StorageCodec.h>
#include <corefw/protocol/Identity.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace corefw::companion {

// FileStore is the minimal flash-file abstraction the store needs. A whole file
// is read or written at once (records are small and bounded), which keeps the
// backend trivial to implement over LittleFS or an in-memory map.
class FileStore {
 public:
  virtual ~FileStore() = default;
  virtual bool exists(const char* path) = 0;
  // read up to cap bytes of `path` into buf; returns bytes read (0 if missing).
  virtual size_t read(const char* path, uint8_t* buf, size_t cap) = 0;
  // overwrite `path` with len bytes; returns success.
  virtual bool write(const char* path, const uint8_t* data, size_t len) = 0;
  virtual bool remove(const char* path) = 0;
  virtual uint32_t usedKb() { return 0; }
  virtual uint32_t totalKb() { return 0; }
};

class PersistentStore {
 public:
  explicit PersistentStore(FileStore& fs) : fs_(fs) {}

  // --- Identity -----------------------------------------------------------
  // Loads /identity/_main.id into `id` (+ optional node name). Returns false if
  // there is no stored identity (caller then generates a fresh one and saves).
  bool loadIdentity(proto::LocalIdentity& id, char* name, size_t name_cap) {
    char path[48];
    identityPath(path);
    if (!fs_.exists(path)) return false;
    uint8_t rec[IDENTITY_RECORD_SIZE];
    size_t n = fs_.read(path, rec, sizeof(rec));
    if (n < proto::PRV_KEY_SIZE) return false;
    decodeIdentity(rec, n, id, name, name_cap);
    return true;
  }
  bool saveIdentity(const proto::LocalIdentity& id, const char* name) {
    char path[48];
    identityPath(path);
    uint8_t rec[IDENTITY_RECORD_SIZE];
    encodeIdentity(id, name, rec);
    return fs_.write(path, rec, sizeof(rec));
  }

  // --- Preferences --------------------------------------------------------
  bool loadPrefs(CompanionState& s) {
    if (!fs_.exists(PREFS_FILE)) return false;
    uint8_t rec[PREFS_RECORD_SIZE];
    if (fs_.read(PREFS_FILE, rec, sizeof(rec)) < PREFS_RECORD_SIZE) return false;
    decodePrefs(rec, s);
    return true;
  }
  bool savePrefs(const CompanionState& s) {
    uint8_t rec[PREFS_RECORD_SIZE];
    encodePrefs(s, rec);
    return fs_.write(PREFS_FILE, rec, sizeof(rec));
  }

  // --- Contacts -----------------------------------------------------------
  // Loads all /contacts3 records into the state's contact table (skipping
  // transient entries the reference never persists is the writer's job).
  bool loadContacts(CompanionState& s) {
    uint8_t buf[kMaxContacts * CONTACT_RECORD_SIZE];
    size_t n = fs_.read(CONTACTS_FILE, buf, sizeof(buf));
    s.num_contacts = 0;
    for (size_t off = 0; off + CONTACT_RECORD_SIZE <= n && s.num_contacts < kMaxContacts;
         off += CONTACT_RECORD_SIZE) {
      decodeContact(&buf[off], s.contacts[s.num_contacts]);
      s.num_contacts++;
    }
    return n > 0;
  }
  bool saveContacts(const CompanionState& s) {
    uint8_t buf[kMaxContacts * CONTACT_RECORD_SIZE];
    size_t off = 0;
    for (int i = 0; i < s.num_contacts; i++) {
      if (s.contacts[i].type == ADV_TYPE_NONE) continue;  // don't persist anon entries
      encodeContact(s.contacts[i], &buf[off]);
      off += CONTACT_RECORD_SIZE;
    }
    return fs_.write(CONTACTS_FILE, buf, off);
  }

  // --- Channels -----------------------------------------------------------
  bool loadChannels(CompanionState& s) {
    uint8_t buf[kMaxGroupChannels * CHANNEL_RECORD_SIZE];
    size_t n = fs_.read(CHANNELS_FILE, buf, sizeof(buf));
    int idx = 0;
    for (size_t off = 0; off + CHANNEL_RECORD_SIZE <= n && idx < kMaxGroupChannels;
         off += CHANNEL_RECORD_SIZE) {
      decodeChannel(&buf[off], s.channels[idx]);
      s.channel_used[idx] = true;
      idx++;
    }
    return n > 0;
  }
  bool saveChannels(const CompanionState& s) {
    uint8_t buf[kMaxGroupChannels * CHANNEL_RECORD_SIZE];
    size_t off = 0;
    // Persist the contiguous run of used channels (matches the reference, which
    // stops at the first unused index).
    for (int i = 0; i < kMaxGroupChannels && s.channel_used[i]; i++) {
      encodeChannel(s.channels[i], &buf[off]);
      off += CHANNEL_RECORD_SIZE;
    }
    return fs_.write(CHANNELS_FILE, buf, off);
  }

  // loadAll pulls identity(+name), prefs, contacts and channels into `s`,
  // creating and persisting a fresh identity only if none exists. Never formats.
  void loadAll(CompanionState& s, const uint8_t seed[proto::SEED_SIZE]) {
    char name[32] = {};
    if (loadIdentity(s.self, name, sizeof(name))) {
      if (name[0]) std::memcpy(s.node_name, name, sizeof(s.node_name));
    } else {
      s.self = proto::LocalIdentity::fromSeed(seed);  // first boot
      saveIdentity(s.self, s.node_name);
    }
    loadPrefs(s);
    loadContacts(s);
    loadChannels(s);
  }

 private:
  void identityPath(char* out) {
    // "/identity/_main.id"
    std::snprintf(out, 48, "%s/%s.id", IDENTITY_DIR, IDENTITY_NAME);
  }
  FileStore& fs_;
};

}  // namespace corefw::companion
