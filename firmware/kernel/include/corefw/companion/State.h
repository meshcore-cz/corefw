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

namespace corefw::companion {

inline constexpr int MAX_CONTACTS = 100;
inline constexpr int MAX_GROUP_CHANNELS = 40;
inline constexpr int OFFLINE_QUEUE_SIZE = 64;
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
  uint8_t max_contacts = MAX_CONTACTS;
  uint8_t max_group_channels = MAX_GROUP_CHANNELS;
  uint32_t rx_delay_base_ms = 0;      // tuning: rx_delay_base * 1000
  uint32_t airtime_factor_ms = 0;     // tuning: airtime_factor * 1000
  uint8_t autoadd_config = 0;
  uint8_t autoadd_max_hops = 0;
  char default_scope_name[32] = {};
  uint8_t default_scope_key[16] = {};
  uint8_t app_target_ver = 0;         // protocol version the connected app speaks
  bool send_unscoped = false;         // CMD_SET_FLOOD_SCOPE_KEY override
  uint8_t send_scope_key[16] = {};

  // --- Stores -------------------------------------------------------------
  ContactInfo contacts[MAX_CONTACTS];
  int num_contacts = 0;
  ChannelDetails channels[MAX_GROUP_CHANNELS];
  bool channel_used[MAX_GROUP_CHANNELS] = {};
  OfflineMessage offline_queue[OFFLINE_QUEUE_SIZE];
  int offline_queue_len = 0;

  const uint8_t* selfPub() const { return self.pub_key; }

  // --- Contact table ------------------------------------------------------
  ContactInfo* lookupContact(const uint8_t* prefix, size_t plen) {
    for (int i = 0; i < num_contacts; i++) {
      if (contacts[i].prefixMatches(prefix, plen)) return &contacts[i];
    }
    return nullptr;
  }
  bool addContact(const ContactInfo& c) {
    if (num_contacts >= MAX_CONTACTS) return false;
    contacts[num_contacts++] = c;
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
    if (idx >= MAX_GROUP_CHANNELS || !channel_used[idx]) return false;
    out = channels[idx];
    return true;
  }
  bool setChannel(uint8_t idx, const ChannelDetails& ch) {
    if (idx >= MAX_GROUP_CHANNELS) return false;
    channels[idx] = ch;
    channel_used[idx] = true;
    return true;
  }

  // --- Offline queue ------------------------------------------------------
  void pushOffline(const uint8_t* frame, int len) {
    if (offline_queue_len >= OFFLINE_QUEUE_SIZE) {  // drop oldest
      for (int i = 0; i < OFFLINE_QUEUE_SIZE - 1; i++) offline_queue[i] = offline_queue[i + 1];
      offline_queue_len = OFFLINE_QUEUE_SIZE - 1;
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
