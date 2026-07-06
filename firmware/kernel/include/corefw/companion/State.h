// CompanionState — the full device configuration and stores for the companion
// role, plus the device-service and mesh-sender seams the command handler needs.
//
// This is the corefw analog of the reference firmware's NodePrefs plus the
// in-RAM contact/channel/offline tables. Everything here is portable so the
// command handler can be exercised on the host; a board supplies persistence and
// the actual radio send path through CompanionHost / MeshSender.
#pragma once

#include <corefw/companion/Contacts.h>
#include <corefw/protocol/Datagram.h>
#include <corefw/protocol/Identity.h>
#include <corefw/protocol/Packet.h>

#include <cstdint>
#include <cstring>

// Table sizes. The generated build may override these with -D MAX_CONTACTS=...
// (the companion module exposes them as options), so honour any such macro and
// fall back to sensible defaults otherwise. The namespaced kMax* constants below
// are what the code uses — a bare MAX_CONTACTS may be a preprocessor macro.
#ifndef MAX_CONTACTS
#define MAX_CONTACTS 100
#endif
#ifndef MAX_GROUP_CHANNELS
#define MAX_GROUP_CHANNELS 40
#endif
#ifndef OFFLINE_QUEUE_SIZE
#define OFFLINE_QUEUE_SIZE 64
#endif

namespace corefw::companion {

inline constexpr int kMaxContacts = MAX_CONTACTS;
inline constexpr int kMaxGroupChannels = MAX_GROUP_CHANNELS;
inline constexpr int kOfflineQueueSize = OFFLINE_QUEUE_SIZE;
inline constexpr uint32_t MAX_SIGN_DATA_LEN = 8 * 1024;

// Advert node type reported in SELF_INFO (a companion identifies as CHAT).
inline constexpr uint8_t ADV_TYPE_NONE = 0;
inline constexpr uint8_t ADV_TYPE_CHAT = 1;
inline constexpr uint8_t ADV_TYPE_REPEATER = 2;
inline constexpr uint8_t ADV_TYPE_ROOM = 3;
inline constexpr uint8_t ADV_TYPE_SENSOR = 4;

// Advert location policies.
inline constexpr uint8_t ADVERT_LOC_NONE = 0;
inline constexpr uint8_t ADVERT_LOC_SHARE = 1;

// autoadd_config bit 0: when the contact table is full, overwrite the oldest
// non-favourite contact instead of rejecting the add (MeshCore
// AUTO_ADD_OVERWRITE_OLDEST). ContactInfo.flags bit 0 marks a favourite, which
// is never evicted.
inline constexpr uint8_t AUTOADD_OVERWRITE_OLDEST = 0x01;
inline constexpr uint8_t CONTACT_FLAG_FAVOURITE = 0x01;

// Request types for CMD_SEND_*_REQ.
inline constexpr uint8_t REQ_TYPE_GET_STATUS = 0x01;
inline constexpr uint8_t REQ_TYPE_GET_TELEMETRY_DATA = 0x03;
inline constexpr uint8_t TELEM_PERM_BASE = 0x01;

// Error codes (second byte of a RESP_CODE_ERR frame).
enum Err : uint8_t {
  ERR_UNSUPPORTED_CMD = 1,
  ERR_NOT_FOUND = 2,
  ERR_TABLE_FULL = 3,
  ERR_BAD_STATE = 4,
  ERR_FILE_IO = 5,
  ERR_ILLEGAL_ARG = 6,
};

// CompanionState holds the device preferences (mirrors NodePrefs). Radio units
// mirror the SELF_INFO frame: freq in kHz, bandwidth in Hz.
struct CompanionState {
  proto::LocalIdentity self;
  char node_name[32] = "corefw";
  int8_t tx_power_dbm = 22;
  int8_t max_tx_power_dbm = 22;
  int32_t lat_e6 = 0;
  int32_t lon_e6 = 0;
  // GPS runtime settings. Kept in RAM (not in the byte-exact /new_prefs record,
  // whose layout leaves no aligned room for a uint32 gps_interval): the board
  // defaults gps_enabled per its capabilities and the app sets these per session.
  uint8_t gps_enabled = 0;        // power the GPS and track our own position
  uint32_t gps_interval = 0;      // seconds between auto self-adverts (0 = off)
  uint32_t ble_pin = 0;
  uint32_t freq_khz = 869525;   // 869.525 MHz
  uint32_t bw_hz = 250000;      // 250 kHz
  uint8_t sf = 11;
  uint8_t cr = 5;
  uint8_t multi_acks = 0;
  uint8_t advert_loc_policy = ADVERT_LOC_NONE;
  uint8_t telemetry_mode_base = 0;
  uint8_t telemetry_mode_loc = 0;
  uint8_t telemetry_mode_env = 0;
  uint8_t manual_add_contacts = 0;
  uint8_t client_repeat = 0;
  uint8_t path_hash_mode = 0;
  uint16_t max_contacts = kMaxContacts;   // DEVICE_INFO reports max_contacts/2
  uint8_t max_group_channels = kMaxGroupChannels;
  uint32_t rx_delay_base_ms = 0;      // tuning: rx_delay_base * 1000
  uint32_t airtime_factor_ms = 0;     // tuning: airtime_factor * 1000
  uint8_t autoadd_config = 0;
  uint8_t autoadd_max_hops = 0;
  char default_scope_name[32] = {};
  uint8_t default_scope_key[16] = {};
  uint8_t app_target_ver = 0;         // protocol version the connected app speaks
  bool send_unscoped = false;         // CMD_SET_FLOOD_SCOPE_KEY override
  uint8_t send_scope_key[16] = {};

  // Lazy GET_CONTACTS sync (MeshCore ContactsIterator — one contact per loop).
  bool contact_sync_active = false;
  int contact_sync_idx = 0;
  uint32_t contact_sync_since = 0;
  uint32_t contact_sync_most_recent = 0;

  // --- Stores -------------------------------------------------------------
  ContactInfo contacts[kMaxContacts];
  int num_contacts = 0;
  ChannelDetails channels[kMaxGroupChannels];
  bool channel_used[kMaxGroupChannels] = {};
  OfflineMessage offline_queue[kOfflineQueueSize];
  int offline_queue_len = 0;

  const uint8_t* selfPub() const { return self.pub_key; }

  // --- Contact table ------------------------------------------------------
  ContactInfo* lookupContact(const uint8_t* prefix, size_t plen) {
    for (int i = 0; i < num_contacts; i++) {
      if (contacts[i].prefixMatches(prefix, plen)) return &contacts[i];
    }
    return nullptr;
  }
  // Adds (or, when full, overwrites) a contact. On a full table with
  // AUTOADD_OVERWRITE_OLDEST set AND allow_evict, evicts the oldest non-favourite
  // contact (matches MeshCore's allocateContactSlot); if `evicted_pub` is non-null
  // and an eviction happens, it receives that contact's 32-byte pub_key and
  // *evicted is set. Returns false when full and nothing may be evicted.
  //
  // allow_evict guards the *destructive* path: only an explicit user add
  // (CMD_ADD_UPDATE_CONTACT) may overwrite. Incoming adverts pass false so a busy
  // mesh can never silently delete the user's curated contacts — a full table
  // just stops auto-adding new discoveries.
  bool addContact(const ContactInfo& c, uint8_t* evicted_pub = nullptr, bool* evicted = nullptr,
                  bool allow_evict = true) {
    if (evicted) *evicted = false;
    if (num_contacts < kMaxContacts) {
      contacts[num_contacts++] = c;
      return true;
    }
    if (!allow_evict || !(autoadd_config & AUTOADD_OVERWRITE_OLDEST)) return false;
    int oldest = -1;
    uint32_t oldest_lastmod = 0xFFFFFFFFu;
    for (int i = 0; i < num_contacts; i++) {
      if ((contacts[i].flags & CONTACT_FLAG_FAVOURITE) != 0) continue;  // keep favourites
      if (contacts[i].lastmod < oldest_lastmod) {
        oldest_lastmod = contacts[i].lastmod;
        oldest = i;
      }
    }
    if (oldest < 0) return false;  // table is all favourites
    if (evicted_pub) std::memcpy(evicted_pub, contacts[oldest].id.pub_key, proto::PUB_KEY_SIZE);
    if (evicted) *evicted = true;
    contacts[oldest] = c;
    return true;
  }
  bool removeContact(const ContactInfo& c) {
    for (int i = 0; i < num_contacts; i++) {
      if (&contacts[i] == &c || contacts[i].prefixMatches(c.id.pub_key, proto::PUB_KEY_SIZE)) {
        for (int j = i; j < num_contacts - 1; j++) contacts[j] = contacts[j + 1];
        num_contacts--;
        return true;
      }
    }
    return false;
  }

  // --- Channel table ------------------------------------------------------
  bool getChannel(uint8_t idx, ChannelDetails& out) const {
    if (idx >= kMaxGroupChannels || !channel_used[idx]) return false;
    out = channels[idx];
    return true;
  }
  bool setChannel(uint8_t idx, const ChannelDetails& ch) {
    if (idx >= kMaxGroupChannels) return false;
    channels[idx] = ch;
    channel_used[idx] = true;
    return true;
  }

  // --- Offline queue ------------------------------------------------------
  void pushOffline(const uint8_t* frame, int len) {
    if (offline_queue_len >= kOfflineQueueSize) {  // drop oldest
      for (int i = 0; i < kOfflineQueueSize - 1; i++) offline_queue[i] = offline_queue[i + 1];
      offline_queue_len = kOfflineQueueSize - 1;
    }
    OfflineMessage& m = offline_queue[offline_queue_len++];
    m.len = len > MAX_FRAME_SIZE ? MAX_FRAME_SIZE : len;
    std::memcpy(m.buf, frame, m.len);
  }
  int popOffline(uint8_t* frame) {
    if (offline_queue_len == 0) return 0;
    int len = offline_queue[0].len;
    std::memcpy(frame, offline_queue[0].buf, len);
    offline_queue_len--;
    for (int i = 0; i < offline_queue_len; i++) offline_queue[i] = offline_queue[i + 1];
    return len;
  }

  // --- Expected-ACK table -------------------------------------------------
  // A circular table of ACK CRCs for messages we've sent, so an incoming
  // PAYLOAD_ACK can be matched to confirm delivery (MeshCore expected_ack_table).
  static constexpr int kExpectedAcks = 16;
  struct PendingAck {
    uint32_t crc = 0;
    uint32_t sent_s = 0;  // rtc seconds at send, for the app's trip-time estimate
    bool used = false;
  };
  PendingAck expected_acks[kExpectedAcks];
  int expected_ack_next = 0;

  // Pending request matching for inbound PAYLOAD_RESPONSE packets. Login/status
  // match on the sender's pub_key prefix (legacy scheme); telemetry/binary match
  // on the request tag. 0 = nothing pending. Mirrors MeshCore's pending_* fields.
  uint32_t pending_login = 0;
  uint32_t pending_status = 0;
  uint32_t pending_telemetry = 0;
  uint32_t pending_req = 0;

  void recordExpectedAck(uint32_t crc, uint32_t now_s) {
    PendingAck& e = expected_acks[expected_ack_next];
    e.crc = crc;
    e.sent_s = now_s;
    e.used = true;
    expected_ack_next = (expected_ack_next + 1) % kExpectedAcks;
  }
  // Returns true and the send time if `crc` matches a pending ack; clears it.
  bool matchExpectedAck(uint32_t crc, uint32_t& sent_s_out) {
    for (int i = 0; i < kExpectedAcks; i++) {
      if (expected_acks[i].used && expected_acks[i].crc == crc) {
        sent_s_out = expected_acks[i].sent_s;
        expected_acks[i].used = false;
        return true;
      }
    }
    return false;
  }
};

// SendOutcome mirrors the reference MSG_SEND_* return codes.
enum SendOutcome : int { SEND_FAILED = 0, SEND_FLOOD = 1, SEND_DIRECT = 2 };

// MeshSender is the seam between the (portable) command handler and the radio
// scheduler. The handler builds packets with the Datagram builders and hands
// them here; the board/dispatcher implements the actual airtime scheduling.
class MeshSender {
 public:
  virtual ~MeshSender() = default;

  // Send a datagram to a contact: flood when out_path_len == OUT_PATH_UNKNOWN,
  // otherwise direct along out_path. Sets est_timeout (ms) and returns outcome.
  virtual int sendToContact(proto::Packet& pkt, const ContactInfo& c, uint32_t& est_timeout) = 0;

  // Flood a group/channel datagram (scoped). Returns success.
  virtual bool sendGroup(proto::Packet& pkt) = 0;

  // Send a direct packet along an explicit path (raw data / trace).
  virtual bool sendDirect(proto::Packet& pkt, const uint8_t* path, uint8_t path_len) = 0;

  // Zero-hop broadcast (self advert / share contact / control data).
  virtual bool sendZeroHop(proto::Packet& pkt) = 0;

  // Send a fully-formed wire packet with a priority (CMD_SEND_RAW_PACKET).
  virtual bool sendRawPacket(const uint8_t* wire, size_t len, uint8_t priority) = 0;

  // Build and send this node's self advert. flood=false means zero-hop.
  virtual bool sendSelfAdvert(bool flood) = 0;

  // Send a PAYLOAD_ACK (`ack`, ack_len bytes) to `to`: direct along its out_path
  // when known, otherwise flood. Acknowledges a received text message.
  virtual bool sendAck(const uint8_t* ack, uint8_t ack_len, const ContactInfo& to) {
    (void)ack; (void)ack_len; (void)to; return false;
  }

  // Serialize this node's self advert packet to `out` (CMD_EXPORT_CONTACT self).
  // Returns the byte length, or 0 on failure.
  virtual uint8_t exportSelfAdvert(uint8_t* out) { (void)out; return 0; }

  // Share a contact's stored advert zero-hop (CMD_SHARE_CONTACT). Returns success.
  virtual bool shareContact(const ContactInfo& c) { (void)c; return false; }

  // Retrieve a contact's stored raw advert blob (CMD_EXPORT_CONTACT). Returns len.
  virtual uint8_t exportContactBlob(const uint8_t* pub_key, uint8_t* out) {
    (void)pub_key; (void)out; return 0;
  }

  // Import an advert packet as if received (CMD_IMPORT_CONTACT loopback).
  virtual bool importAdvert(const uint8_t* data, size_t len) { (void)data; (void)len; return false; }

  // Connection tracking for room/repeater logins.
  virtual bool hasConnection(const uint8_t* pub_key) { (void)pub_key; return false; }
  virtual void stopConnection(const uint8_t* pub_key) { (void)pub_key; }

  // A monotonically-unique RTC timestamp (tags / replay protection).
  virtual uint32_t rtcNowUnique() = 0;
  // Uniformly random 32-bit value (request/packet-hash uniqueness).
  virtual uint32_t random32() = 0;
};

}  // namespace corefw::companion
