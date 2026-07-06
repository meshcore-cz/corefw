// Companion Protocol command handling.
//
// Parses inbound app->device command frames and produces byte-identical
// device->app response frames, matching the reference companion firmware
// (examples/companion_radio/MyMesh.cpp handleCmdFrame). Device state, contact /
// channel stores and the radio send path are reached through CompanionState,
// CompanionHost and MeshSender, so this whole layer is portable and
// host-testable — the frames it emits are exactly what existing MeshCore apps
// expect. Every CMD_* code the reference implements is handled here.
#pragma once

#include <corefw/companion/Protocol.h>
#include <corefw/companion/State.h>
#include <corefw/protocol/Datagram.h>
#include <corefw/protocol/Wire.h>
#include <corefw/runtime/Airtime.h>  // timeOnAirMs, for the trace round-trip estimate

#include <cstring>

namespace corefw::companion {

// Protocol version reported to the app. This is the compatibility knob apps key
// their behaviour on; it tracks the MeshCore companion protocol version.
inline constexpr uint8_t FIRMWARE_VER_CODE = 13;
inline constexpr const char* FIRMWARE_VERSION = "corefw-0.1.0";
inline constexpr const char* FIRMWARE_BUILD_DATE = "2026";
inline constexpr int8_t MIN_LORA_TX_POWER = -9;

// FrameWriter receives each device->app frame the handler emits. A command may
// emit zero, one or many frames (e.g. GET_CONTACTS streams a start frame, one
// per contact, then an end frame).
class FrameWriter {
 public:
  virtual ~FrameWriter() = default;
  virtual void writeFrame(const uint8_t* data, size_t len) = 0;
};

// CompanionHost bridges the handler to device services (RTC, battery, storage,
// persistence, stats). Defaults are no-ops so host tests override only what they
// assert on.
class CompanionHost {
 public:
  virtual ~CompanionHost() = default;
  virtual uint32_t rtcNow() = 0;                 // UNIX seconds
  virtual void setRtc(uint32_t secs) = 0;
  virtual uint16_t batteryMilliVolts() = 0;
  virtual uint32_t storageUsedKb() { return 0; }
  virtual uint32_t storageTotalKb() { return 0; }
  virtual const char* manufacturerName() { return "corefw"; }
  virtual void savePrefs() {}
  virtual void saveContacts() {}
  virtual void saveChannels() {}
  virtual void applyRadioParams() {}   // freq/bw/sf/cr changed on the driver
  virtual void applyTxPower() {}
  virtual void reboot() {}
  virtual bool factoryReset() { return false; }
  virtual bool privateKeyExportEnabled() { return false; }
  virtual bool privateKeyImportEnabled() { return false; }

  // Custom vars (sensor settings). getCustomVars writes a "name:val,name:val"
  // string; setCustomVar assigns one. Defaults: none.
  virtual size_t getCustomVars(char* out, size_t cap) { (void)out; (void)cap; return 0; }
  virtual bool setCustomVar(const char* name, const char* value) {
    (void)name; (void)value; return false;
  }

  // Stats sub-reports (CMD_GET_STATS). Defaults report zeros.
  virtual void coreStats(uint16_t& batt_mv, uint32_t& uptime_s, uint16_t& err_flags,
                         uint8_t& queue_len) {
    batt_mv = batteryMilliVolts(); uptime_s = 0; err_flags = 0; queue_len = 0;
  }
  virtual void radioStats(int16_t& noise_floor, int8_t& last_rssi, int8_t& last_snr_q4,
                          uint32_t& tx_air_s, uint32_t& rx_air_s) {
    noise_floor = radioNoiseFloorDbm();
    last_rssi = 0; last_snr_q4 = 0; tx_air_s = 0; rx_air_s = 0;
  }
  virtual int16_t radioNoiseFloorDbm() const { return 0; }
  virtual void packetStats(uint32_t out[7]) { std::memset(out, 0, 7 * sizeof(uint32_t)); }

  // GPS status for the on-screen indicator. Boards without GPS keep the default
  // (disabled); positions live in CompanionState (lat_e6/lon_e6).
  virtual bool gpsEnabled() const { return false; }
  virtual bool gpsHasFix() const { return false; }
  virtual uint8_t gpsSatellites() const { return 0; }

  // GET_ADVERT_PATH: return path length + recv timestamp for a pubkey prefix, or
  // -1 if unknown.
  virtual int advertPath(const uint8_t* pub_key, uint32_t& recv_ts, uint8_t* path) {
    (void)pub_key; (void)recv_ts; (void)path; return -1;
  }
  // Allowed client-repeat frequency ranges. Returns count; fills lower/upper.
  virtual int repeatFreqRanges(uint32_t* lowers, uint32_t* uppers, int max) {
    (void)lowers; (void)uppers; (void)max; return 0;
  }
  virtual bool isValidClientRepeatFreq(uint32_t freq) { (void)freq; return false; }
};

// CommandHandler processes decoded inbound frames and writes response frames.
class CommandHandler {
 public:
  CommandHandler(CompanionState& state, CompanionHost& host, MeshSender& sender)
      : s_(state), h_(host), tx_(sender) {}

  // handle processes one inbound command payload (cmd[0] is the code) and writes
  // any reply frames to `out`. Unknown or malformed commands yield an ERR frame.
  void handle(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len == 0) return;
    switch (cmd[0]) {
      case CMD_DEVICE_QUERY:      return len >= 2 ? deviceInfo(cmd[1], out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_APP_START:         return len >= 8 ? selfInfo(out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_GET_DEVICE_TIME:   return currTime(out);
      case CMD_SET_DEVICE_TIME:   return len >= 5 ? setTime(cmd, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_SET_ADVERT_NAME:   return len >= 2 ? setName(cmd, len, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_SET_ADVERT_LATLON: return len >= 9 ? setLatLon(cmd, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_SET_RADIO_TX_POWER:return len >= 2 ? setTxPower(cmd, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_SET_RADIO_PARAMS:  return setRadioParams(cmd, len, out);
      case CMD_SET_TUNING_PARAMS: return len >= 9 ? setTuning(cmd, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_GET_TUNING_PARAMS: return getTuning(out);
      case CMD_SET_OTHER_PARAMS:  return len >= 2 ? setOtherParams(cmd, len, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_SET_PATH_HASH_MODE:return setPathHashMode(cmd, len, out);
      case CMD_SET_DEVICE_PIN:    return len >= 5 ? setDevicePin(cmd, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_GET_BATT_AND_STORAGE: return battAndStorage(out);
      case CMD_SEND_SELF_ADVERT:  return sendSelfAdvert(cmd, len, out);
      case CMD_REBOOT:            return reboot(cmd, len, out);
      case CMD_FACTORY_RESET:     return factoryReset(cmd, len, out);
      case CMD_SET_AUTOADD_CONFIG:return len >= 2 ? setAutoAdd(cmd, len, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_GET_AUTOADD_CONFIG:return getAutoAdd(out);
      case CMD_GET_ALLOWED_REPEAT_FREQ: return getRepeatFreq(out);
      case CMD_SET_DEFAULT_FLOOD_SCOPE: return len >= 1 ? setDefaultScope(cmd, len, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_GET_DEFAULT_FLOOD_SCOPE: return getDefaultScope(out);
      case CMD_SET_FLOOD_SCOPE_KEY: return setScopeKey(cmd, len, out);
      case CMD_EXPORT_PRIVATE_KEY:return exportPrivateKey(out);
      case CMD_IMPORT_PRIVATE_KEY:return importPrivateKey(cmd, len, out);
      case CMD_GET_STATS:         return len >= 2 ? getStats(cmd[1], out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_GET_CUSTOM_VARS:   return getCustomVars(out);
      case CMD_SET_CUSTOM_VAR:    return len >= 4 ? setCustomVar(cmd, len, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_GET_ADVERT_PATH:   return len >= proto::PUB_KEY_SIZE + 2 ? getAdvertPath(cmd, out) : err(ERR_ILLEGAL_ARG, out);

      // --- Contacts ---
      case CMD_GET_CONTACTS:      return getContacts(cmd, len, out);
      case CMD_GET_CONTACT_BY_KEY:return getContactByKey(cmd, len, out);
      case CMD_ADD_UPDATE_CONTACT:return addUpdateContact(cmd, len, out);
      case CMD_REMOVE_CONTACT:    return removeContactCmd(cmd, len, out);
      case CMD_RESET_PATH:        return resetPath(cmd, len, out);
      case CMD_SHARE_CONTACT:     return shareContactCmd(cmd, len, out);
      case CMD_EXPORT_CONTACT:    return exportContact(cmd, len, out);
      case CMD_IMPORT_CONTACT:    return importContact(cmd, len, out);

      // --- Channels ---
      case CMD_GET_CHANNEL:       return len >= 2 ? getChannelCmd(cmd, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_SET_CHANNEL:       return setChannelCmd(cmd, len, out);

      // --- Messaging ---
      case CMD_SEND_TXT_MSG:      return sendTxtMsg(cmd, len, out);
      case CMD_SEND_CHANNEL_TXT_MSG: return sendChannelTxt(cmd, len, out);
      case CMD_SEND_CHANNEL_DATA: return sendChannelData(cmd, len, out);
      case CMD_SYNC_NEXT_MESSAGE: return syncNextMessage(out);

      // --- Requests / connections ---
      case CMD_SEND_LOGIN:        return sendLogin(cmd, len, out);
      case CMD_SEND_ANON_REQ:     return sendAnonReq(cmd, len, out);
      case CMD_SEND_STATUS_REQ:   return sendStatusReq(cmd, len, out);
      case CMD_SEND_TELEMETRY_REQ:return sendTelemetryReq(cmd, len, out);
      case CMD_SEND_BINARY_REQ:   return sendBinaryReq(cmd, len, out);
      case CMD_SEND_PATH_DISCOVERY_REQ: return sendPathDiscovery(cmd, len, out);
      case CMD_HAS_CONNECTION:    return hasConnectionCmd(cmd, len, out);
      case CMD_LOGOUT:            return logout(cmd, len, out);

      // --- Raw / trace / control ---
      case CMD_SEND_RAW_DATA:     return sendRawData(cmd, len, out);
      case CMD_SEND_RAW_PACKET:   return sendRawPacketCmd(cmd, len, out);
      case CMD_SEND_TRACE_PATH:   return sendTracePath(cmd, len, out);
      case CMD_SEND_CONTROL_DATA: return sendControlData(cmd, len, out);

      // --- Signing ---
      case CMD_SIGN_START:        return signStart(out);
      case CMD_SIGN_DATA:         return signData(cmd, len, out);
      case CMD_SIGN_FINISH:       return signFinish(out);

      default:                    return err(ERR_UNSUPPORTED_CMD, out);
    }
  }

  bool contactSyncActive() const { return s_.contact_sync_active; }

  // Send at most one CONTACT (or END_OF_CONTACTS) per call — matches
  // MyMesh::checkSerialInterface() while the contacts iterator is running.
  void pumpContactSync(FrameWriter& out) {
    if (!s_.contact_sync_active) return;
    while (s_.contact_sync_idx < s_.num_contacts) {
      ContactInfo& c = s_.contacts[s_.contact_sync_idx++];
      if (c.lastmod < s_.contact_sync_since) continue;
      uint8_t f[160];
      size_t n = writeContactRespFrame(RESP_CODE_CONTACT, c, f);
      out.writeFrame(f, n);
      if (c.lastmod > s_.contact_sync_most_recent) s_.contact_sync_most_recent = c.lastmod;
      return;
    }
    uint8_t endf[5];
    endf[0] = RESP_CODE_END_OF_CONTACTS;
    proto::putU32LE(endf, 1, s_.contact_sync_most_recent);
    out.writeFrame(endf, 5);
    s_.contact_sync_active = false;
  }

 private:
  // --- Frame helpers ------------------------------------------------------
  void ok(FrameWriter& out) { uint8_t f[1] = {RESP_CODE_OK}; out.writeFrame(f, 1); }
  void err(uint8_t code, FrameWriter& out) { uint8_t f[2] = {RESP_CODE_ERR, code}; out.writeFrame(f, 2); }
  void disabled(FrameWriter& out) { uint8_t f[1] = {RESP_CODE_DISABLED}; out.writeFrame(f, 1); }

  // RESP_CODE_SENT: [code, flood?1:0, tag(4), est_timeout(4)] = 10 bytes.
  void sent(int outcome, uint32_t tag, uint32_t est_timeout, FrameWriter& out) {
    uint8_t f[10];
    f[0] = RESP_CODE_SENT;
    f[1] = uint8_t(outcome == SEND_FLOOD ? 1 : 0);
    proto::putU32LE(f, 2, tag);
    proto::putU32LE(f, 6, est_timeout);
    out.writeFrame(f, 10);
  }

  // --- Device info / config ----------------------------------------------
  void deviceInfo(uint8_t appVer, FrameWriter& out) {
    s_.app_target_ver = appVer;
    uint8_t f[96];  // DEVICE_INFO frame is 82 bytes
    size_t i = 0;
    f[i++] = RESP_CODE_DEVICE_INFO;
    f[i++] = FIRMWARE_VER_CODE;
    f[i++] = uint8_t(s_.max_contacts / 2);
    f[i++] = s_.max_group_channels;
    i = proto::putU32LE(f, i, s_.ble_pin);
    std::memset(&f[i], 0, 12);
    strzcpy(reinterpret_cast<char*>(&f[i]), FIRMWARE_BUILD_DATE, 12); i += 12;
    strzcpy(reinterpret_cast<char*>(&f[i]), h_.manufacturerName(), 40); i += 40;
    strzcpy(reinterpret_cast<char*>(&f[i]), FIRMWARE_VERSION, 20); i += 20;
    f[i++] = s_.client_repeat;
    f[i++] = s_.path_hash_mode;
    out.writeFrame(f, i);
  }

  void selfInfo(FrameWriter& out) {
    s_.contact_sync_active = false;
    uint8_t f[128];
    size_t i = 0;
    f[i++] = RESP_CODE_SELF_INFO;
    f[i++] = ADV_TYPE_CHAT;
    f[i++] = uint8_t(s_.tx_power_dbm);
    f[i++] = uint8_t(s_.max_tx_power_dbm);
    std::memcpy(&f[i], s_.self.pub_key, proto::PUB_KEY_SIZE); i += proto::PUB_KEY_SIZE;
    i = proto::putU32LE(f, i, uint32_t(s_.lat_e6));
    i = proto::putU32LE(f, i, uint32_t(s_.lon_e6));
    f[i++] = s_.multi_acks;
    f[i++] = s_.advert_loc_policy;
    f[i++] = uint8_t((s_.telemetry_mode_env << 4) | (s_.telemetry_mode_loc << 2) | s_.telemetry_mode_base);
    f[i++] = s_.manual_add_contacts;
    i = proto::putU32LE(f, i, s_.freq_khz);
    i = proto::putU32LE(f, i, s_.bw_hz);
    f[i++] = s_.sf;
    f[i++] = s_.cr;
    size_t nlen = std::strlen(s_.node_name);
    std::memcpy(&f[i], s_.node_name, nlen); i += nlen;
    out.writeFrame(f, i);
  }

  void currTime(FrameWriter& out) {
    uint8_t f[5]; f[0] = RESP_CODE_CURR_TIME; proto::putU32LE(f, 1, h_.rtcNow());
    out.writeFrame(f, 5);
  }
  void setTime(const uint8_t* cmd, FrameWriter& out) {
    uint32_t secs = proto::getU32LE(cmd, 1);
    if (secs >= h_.rtcNow()) { h_.setRtc(secs); ok(out); } else err(ERR_ILLEGAL_ARG, out);
  }
  void setName(const uint8_t* cmd, size_t len, FrameWriter& out) {
    size_t nlen = len - 1;
    if (nlen > sizeof(s_.node_name) - 1) nlen = sizeof(s_.node_name) - 1;
    std::memcpy(s_.node_name, &cmd[1], nlen);
    s_.node_name[nlen] = 0;
    h_.savePrefs();
    ok(out);
  }
  void setLatLon(const uint8_t* cmd, FrameWriter& out) {
    int32_t lat = int32_t(proto::getU32LE(cmd, 1));
    int32_t lon = int32_t(proto::getU32LE(cmd, 5));
    if (lat <= 90000000 && lat >= -90000000 && lon <= 180000000 && lon >= -180000000) {
      s_.lat_e6 = lat; s_.lon_e6 = lon; h_.savePrefs(); ok(out);
    } else err(ERR_ILLEGAL_ARG, out);
  }
  void setTxPower(const uint8_t* cmd, FrameWriter& out) {
    int8_t p = int8_t(cmd[1]);
    if (p < MIN_LORA_TX_POWER || p > s_.max_tx_power_dbm) { err(ERR_ILLEGAL_ARG, out); return; }
    s_.tx_power_dbm = p; h_.savePrefs(); h_.applyTxPower(); ok(out);
  }
  void setRadioParams(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 11) { err(ERR_ILLEGAL_ARG, out); return; }
    uint32_t freq = proto::getU32LE(cmd, 1);
    uint32_t bw = proto::getU32LE(cmd, 5);
    uint8_t sf = cmd[9], cr = cmd[10];
    uint8_t repeat = len > 11 ? cmd[11] : 0;
    if (repeat && !h_.isValidClientRepeatFreq(freq)) { err(ERR_ILLEGAL_ARG, out); return; }
    if (freq >= 150000 && freq <= 2500000 && sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8 &&
        bw >= 7000 && bw <= 500000) {
      s_.freq_khz = freq; s_.bw_hz = bw; s_.sf = sf; s_.cr = cr; s_.client_repeat = repeat;
      h_.savePrefs(); h_.applyRadioParams(); ok(out);
    } else err(ERR_ILLEGAL_ARG, out);
  }
  void setTuning(const uint8_t* cmd, FrameWriter& out) {
    s_.rx_delay_base_ms = proto::getU32LE(cmd, 1);
    s_.airtime_factor_ms = proto::getU32LE(cmd, 5);
    h_.savePrefs(); ok(out);
  }
  void getTuning(FrameWriter& out) {
    uint8_t f[9]; f[0] = RESP_CODE_TUNING_PARAMS;
    proto::putU32LE(f, 1, s_.rx_delay_base_ms);
    proto::putU32LE(f, 5, s_.airtime_factor_ms);
    out.writeFrame(f, 9);
  }
  void setOtherParams(const uint8_t* cmd, size_t len, FrameWriter& out) {
    s_.manual_add_contacts = cmd[1];
    if (len >= 3) {
      s_.telemetry_mode_base = cmd[2] & 0x03;
      s_.telemetry_mode_loc = (cmd[2] >> 2) & 0x03;
      s_.telemetry_mode_env = (cmd[2] >> 4) & 0x03;
      if (len >= 4) {
        s_.advert_loc_policy = cmd[3];
        if (len >= 5) s_.multi_acks = cmd[4];
      }
    }
    h_.savePrefs(); ok(out);
  }
  void setPathHashMode(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 3 || cmd[1] != 0) { err(ERR_ILLEGAL_ARG, out); return; }
    if (cmd[2] >= 3) { err(ERR_ILLEGAL_ARG, out); return; }
    s_.path_hash_mode = cmd[2]; h_.savePrefs(); ok(out);
  }
  void setDevicePin(const uint8_t* cmd, FrameWriter& out) {
    uint32_t pin = proto::getU32LE(cmd, 1);
    if (pin == 0 || (pin >= 100000 && pin <= 999999)) {
      s_.ble_pin = pin; h_.savePrefs(); ok(out);
    } else err(ERR_ILLEGAL_ARG, out);
  }
  void battAndStorage(FrameWriter& out) {
    uint8_t f[11]; size_t i = 0;
    f[i++] = RESP_CODE_BATT_AND_STORAGE;
    i = proto::putU16LE(f, i, h_.batteryMilliVolts());
    i = proto::putU32LE(f, i, h_.storageUsedKb());
    i = proto::putU32LE(f, i, h_.storageTotalKb());
    out.writeFrame(f, i);
  }
  void sendSelfAdvert(const uint8_t* cmd, size_t len, FrameWriter& out) {
    bool flood = len >= 2 && cmd[1] == 1;
    if (tx_.sendSelfAdvert(flood)) ok(out); else err(ERR_TABLE_FULL, out);
  }
  void reboot(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len >= 7 && std::memcmp(&cmd[1], "reboot", 6) == 0) { h_.saveContacts(); h_.reboot(); }
    else err(ERR_ILLEGAL_ARG, out);
  }
  void factoryReset(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len >= 6 && std::memcmp(&cmd[1], "reset", 5) == 0) {
      if (h_.factoryReset()) { ok(out); h_.reboot(); } else err(ERR_FILE_IO, out);
    } else err(ERR_ILLEGAL_ARG, out);
  }
  void setAutoAdd(const uint8_t* cmd, size_t len, FrameWriter& out) {
    s_.autoadd_config = cmd[1];
    if (len >= 3) s_.autoadd_max_hops = cmd[2] < 64 ? cmd[2] : 64;
    h_.savePrefs(); ok(out);
  }
  void getAutoAdd(FrameWriter& out) {
    uint8_t f[3] = {RESP_CODE_AUTOADD_CONFIG, s_.autoadd_config, s_.autoadd_max_hops};
    out.writeFrame(f, 3);
  }
  void getRepeatFreq(FrameWriter& out) {
    uint8_t f[MAX_FRAME_SIZE]; size_t i = 0;
    f[i++] = RESP_ALLOWED_REPEAT_FREQ;
    uint32_t lo[16], hi[16];
    int n = h_.repeatFreqRanges(lo, hi, 16);
    for (int k = 0; k < n && i + 8 < sizeof(f); k++) {
      i = proto::putU32LE(f, i, lo[k]);
      i = proto::putU32LE(f, i, hi[k]);
    }
    out.writeFrame(f, i);
  }
  void setDefaultScope(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len >= 1 + 31 + 16) {
      size_t n = strnlen_(reinterpret_cast<const char*>(&cmd[1]), 31);
      if (n > 0 && n < 31) {
        std::memset(s_.default_scope_name, 0, sizeof(s_.default_scope_name));
        std::memcpy(s_.default_scope_name, &cmd[1], n);
        std::memcpy(s_.default_scope_key, &cmd[1 + 31], 16);
        h_.savePrefs(); ok(out);
      } else err(ERR_ILLEGAL_ARG, out);
    } else {
      std::memset(s_.default_scope_name, 0, sizeof(s_.default_scope_name));
      std::memset(s_.default_scope_key, 0, sizeof(s_.default_scope_key));
      h_.savePrefs(); ok(out);
    }
  }
  void getDefaultScope(FrameWriter& out) {
    uint8_t f[1 + 31 + 16]; f[0] = RESP_CODE_DEFAULT_FLOOD_SCOPE;
    if (std::strlen(s_.default_scope_name) > 0) {
      std::memcpy(&f[1], s_.default_scope_name, 31);
      std::memcpy(&f[1 + 31], s_.default_scope_key, 16);
      out.writeFrame(f, 1 + 31 + 16);
    } else out.writeFrame(f, 1);
  }
  void setScopeKey(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len >= 2 && cmd[1] == 0) {
      if (len >= 2 + 16) std::memcpy(s_.send_scope_key, &cmd[2], 16);
      else std::memset(s_.send_scope_key, 0, 16);
      s_.send_unscoped = false; ok(out);
    } else if (len >= 2 && cmd[1] == 1) {
      s_.send_unscoped = true; ok(out);
    } else err(ERR_ILLEGAL_ARG, out);
  }
  void exportPrivateKey(FrameWriter& out) {
    if (!h_.privateKeyExportEnabled()) { disabled(out); return; }
    uint8_t f[65]; f[0] = RESP_CODE_PRIVATE_KEY;
    s_.self.writeTo(&f[1], 64); out.writeFrame(f, 65);
  }
  void importPrivateKey(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (!h_.privateKeyImportEnabled()) { disabled(out); return; }
    if (len < 65) { err(ERR_ILLEGAL_ARG, out); return; }
    s_.self.readFrom(&cmd[1], 64);
    h_.savePrefs(); ok(out);
  }
  void getStats(uint8_t type, FrameWriter& out) {
    uint8_t f[64]; size_t i = 0;
    if (type == STATS_TYPE_CORE) {
      f[i++] = RESP_CODE_STATS; f[i++] = STATS_TYPE_CORE;
      uint16_t mv, ef; uint32_t up; uint8_t ql;
      h_.coreStats(mv, up, ef, ql);
      i = proto::putU16LE(f, i, mv);
      i = proto::putU32LE(f, i, up);
      i = proto::putU16LE(f, i, ef);
      f[i++] = ql;
      out.writeFrame(f, i);
    } else if (type == STATS_TYPE_RADIO) {
      f[i++] = RESP_CODE_STATS; f[i++] = STATS_TYPE_RADIO;
      int16_t nf; int8_t rssi, snr; uint32_t tx, rx;
      h_.radioStats(nf, rssi, snr, tx, rx);
      i = proto::putU16LE(f, i, uint16_t(nf));
      f[i++] = uint8_t(rssi); f[i++] = uint8_t(snr);
      i = proto::putU32LE(f, i, tx);
      i = proto::putU32LE(f, i, rx);
      out.writeFrame(f, i);
    } else if (type == STATS_TYPE_PACKETS) {
      f[i++] = RESP_CODE_STATS; f[i++] = STATS_TYPE_PACKETS;
      uint32_t p[7]; h_.packetStats(p);
      for (int k = 0; k < 7; k++) i = proto::putU32LE(f, i, p[k]);
      out.writeFrame(f, i);
    } else err(ERR_ILLEGAL_ARG, out);
  }
  void getCustomVars(FrameWriter& out) {
    uint8_t f[MAX_FRAME_SIZE]; f[0] = RESP_CODE_CUSTOM_VARS;
    size_t n = h_.getCustomVars(reinterpret_cast<char*>(&f[1]), MAX_FRAME_SIZE - 1);
    out.writeFrame(f, 1 + n);
  }
  void setCustomVar(const uint8_t* cmd, size_t len, FrameWriter& out) {
    char buf[MAX_FRAME_SIZE];
    size_t n = len - 1; if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    std::memcpy(buf, &cmd[1], n); buf[n] = 0;
    char* sep = std::strchr(buf, ':');
    if (!sep) { err(ERR_ILLEGAL_ARG, out); return; }
    *sep = 0;
    if (h_.setCustomVar(buf, sep + 1)) ok(out); else err(ERR_ILLEGAL_ARG, out);
  }
  void getAdvertPath(const uint8_t* cmd, FrameWriter& out) {
    const uint8_t* pub = &cmd[2];
    uint32_t recv_ts = 0; uint8_t path[proto::MAX_PATH_SIZE];
    int plen = h_.advertPath(pub, recv_ts, path);
    if (plen < 0) { err(ERR_NOT_FOUND, out); return; }
    uint8_t f[6 + proto::MAX_PATH_SIZE]; size_t i = 0;
    f[i++] = RESP_CODE_ADVERT_PATH;
    i = proto::putU32LE(f, i, recv_ts);
    f[i++] = uint8_t(plen);
    std::memcpy(&f[i], path, plen); i += plen;
    out.writeFrame(f, i);
  }

  // --- Contacts -----------------------------------------------------------
  void getContacts(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (s_.contact_sync_active) { err(ERR_BAD_STATE, out); return; }
    uint32_t since = len >= 5 ? proto::getU32LE(cmd, 1) : 0;
    uint8_t start[5];
    start[0] = RESP_CODE_CONTACTS_START;
    proto::putU32LE(start, 1, uint32_t(s_.num_contacts));
    out.writeFrame(start, 5);
    s_.contact_sync_active = true;
    s_.contact_sync_idx = 0;
    s_.contact_sync_since = since;
    s_.contact_sync_most_recent = 0;
  }
  void getContactByKey(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 1 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    ContactInfo* c = s_.lookupContact(&cmd[1], proto::PUB_KEY_SIZE);
    if (!c) { err(ERR_NOT_FOUND, out); return; }
    uint8_t f[160]; size_t n = writeContactRespFrame(RESP_CODE_CONTACT, *c, f);
    out.writeFrame(f, n);
  }
  void addUpdateContact(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 1 + 32 + 2 + 1) { err(ERR_ILLEGAL_ARG, out); return; }
    uint32_t last_mod = h_.rtcNow();
    ContactInfo* existing = s_.lookupContact(&cmd[1], proto::PUB_KEY_SIZE);
    if (existing) {
      updateContactFromFrame(*existing, last_mod, cmd, int(len));
      existing->lastmod = last_mod; h_.saveContacts(); ok(out);
    } else {
      ContactInfo c;
      updateContactFromFrame(c, last_mod, cmd, int(len));
      c.lastmod = last_mod; c.sync_since = 0;
      uint8_t evicted[proto::PUB_KEY_SIZE];
      bool did_evict = false;
      if (s_.addContact(c, evicted, &did_evict)) {
        // Tell the app which contact was overwritten so its list stays in sync.
        if (did_evict) {
          uint8_t d[1 + proto::PUB_KEY_SIZE];
          d[0] = PUSH_CODE_CONTACT_DELETED;
          std::memcpy(&d[1], evicted, proto::PUB_KEY_SIZE);
          out.writeFrame(d, sizeof(d));
        }
        h_.saveContacts();
        ok(out);
      } else err(ERR_TABLE_FULL, out);
    }
  }
  void removeContactCmd(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 1 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    ContactInfo* c = s_.lookupContact(&cmd[1], proto::PUB_KEY_SIZE);
    if (c && s_.removeContact(*c)) { h_.saveContacts(); ok(out); } else err(ERR_NOT_FOUND, out);
  }
  void resetPath(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 1 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    ContactInfo* c = s_.lookupContact(&cmd[1], proto::PUB_KEY_SIZE);
    if (c) { c->out_path_len = OUT_PATH_UNKNOWN; h_.saveContacts(); ok(out); }
    else err(ERR_NOT_FOUND, out);
  }
  void shareContactCmd(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 1 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    ContactInfo* c = s_.lookupContact(&cmd[1], proto::PUB_KEY_SIZE);
    if (!c) { err(ERR_NOT_FOUND, out); return; }
    if (tx_.shareContact(*c)) ok(out); else err(ERR_TABLE_FULL, out);
  }
  void exportContact(const uint8_t* cmd, size_t len, FrameWriter& out) {
    uint8_t f[MAX_FRAME_SIZE]; f[0] = RESP_CODE_EXPORT_CONTACT;
    if (len < 1 + proto::PUB_KEY_SIZE) {
      uint8_t n = tx_.exportSelfAdvert(&f[1]);
      if (n > 0) out.writeFrame(f, n + 1); else err(ERR_TABLE_FULL, out);
    } else {
      uint8_t n = tx_.exportContactBlob(&cmd[1], &f[1]);
      if (n > 0) out.writeFrame(f, n + 1); else err(ERR_NOT_FOUND, out);
    }
  }
  void importContact(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len <= 2 + 32 + 64) { err(ERR_ILLEGAL_ARG, out); return; }
    if (tx_.importAdvert(&cmd[1], len - 1)) ok(out); else err(ERR_ILLEGAL_ARG, out);
  }

  // --- Channels -----------------------------------------------------------
  void getChannelCmd(const uint8_t* cmd, FrameWriter& out) {
    uint8_t idx = cmd[1];
    ChannelDetails ch;
    if (!s_.getChannel(idx, ch)) { err(ERR_NOT_FOUND, out); return; }
    uint8_t f[1 + 1 + 32 + 16]; size_t i = 0;
    f[i++] = RESP_CODE_CHANNEL_INFO;
    f[i++] = idx;
    std::memset(&f[i], 0, 32);
    std::strncpy(reinterpret_cast<char*>(&f[i]), ch.name, 31);
    i += 32;
    std::memcpy(&f[i], ch.channel.secret, 16); i += 16;
    out.writeFrame(f, i);
  }
  void setChannelCmd(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len >= 2 + 32 + 32) { err(ERR_UNSUPPORTED_CMD, out); return; }  // 256-bit not supported
    if (len < 2 + 32 + 16) { err(ERR_ILLEGAL_ARG, out); return; }
    uint8_t idx = cmd[1];
    ChannelDetails ch;
    std::memset(ch.name, 0, sizeof(ch.name));
    std::memcpy(ch.name, &cmd[2], 32); ch.name[31] = 0;
    uint8_t key[16]; std::memcpy(key, &cmd[2 + 32], 16);
    ch.channel.setSecret(key);
    if (s_.setChannel(idx, ch)) { h_.saveChannels(); ok(out); } else err(ERR_NOT_FOUND, out);
  }

  // --- Messaging ----------------------------------------------------------
  void sendTxtMsg(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 14) { err(ERR_ILLEGAL_ARG, out); return; }
    uint8_t txt_type = cmd[1];
    uint8_t attempt = cmd[2];
    uint32_t msg_timestamp = proto::getU32LE(cmd, 3);
    const uint8_t* prefix = &cmd[7];  // 6-byte pubkey prefix
    ContactInfo* c = s_.lookupContact(prefix, 6);
    if (!c || (txt_type != proto::TXT_TYPE_PLAIN && txt_type != proto::TXT_TYPE_CLI_DATA)) {
      err(c == nullptr ? ERR_NOT_FOUND : ERR_UNSUPPORTED_CMD, out);
      return;
    }
    char text[proto::MAX_TEXT_LEN + 1];
    size_t tlen = len - 13;
    if (tlen > proto::MAX_TEXT_LEN) tlen = proto::MAX_TEXT_LEN;
    std::memcpy(text, &cmd[13], tlen); text[tlen] = 0;

    uint8_t secret[proto::PUB_KEY_SIZE];
    s_.self.calcSharedSecret(secret, c->id);
    uint8_t temp[5 + proto::MAX_TEXT_LEN + 2];
    uint32_t expected_ack = 0;
    size_t plen;
    if (txt_type == proto::TXT_TYPE_CLI_DATA) {
      msg_timestamp = tx_.rtcNowUnique();
      plen = proto::composeCliDataPlaintext(temp, msg_timestamp, attempt, text);
    } else {
      plen = proto::composeTextPlaintext(temp, msg_timestamp, attempt, text, s_.self, expected_ack);
    }
    proto::Packet pkt;
    if (!proto::buildDatagram(pkt, proto::PAYLOAD_TXT_MSG, c->id, s_.self, secret, temp, plen)) {
      err(ERR_TABLE_FULL, out); return;
    }
    uint32_t est_timeout = 0;
    int outcome = tx_.sendToContact(pkt, *c, est_timeout);
    if (outcome == SEND_FAILED) { err(ERR_TABLE_FULL, out); return; }
    // Remember the expected ACK so an incoming PAYLOAD_ACK confirms delivery.
    if (expected_ack != 0) s_.recordExpectedAck(expected_ack, h_.rtcNow());
    sent(outcome, expected_ack, est_timeout, out);
  }

  void sendChannelTxt(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 6) { err(ERR_ILLEGAL_ARG, out); return; }
    uint8_t txt_type = cmd[1];
    uint8_t channel_idx = cmd[2];
    uint32_t msg_timestamp = proto::getU32LE(cmd, 3);
    if (txt_type != proto::TXT_TYPE_PLAIN) { err(ERR_UNSUPPORTED_CMD, out); return; }
    ChannelDetails ch;
    if (!s_.getChannel(channel_idx, ch)) { err(ERR_NOT_FOUND, out); return; }
    size_t text_len = len - 7;
    uint8_t temp[5 + proto::MAX_TEXT_LEN + 32];
    size_t plen = proto::composeGroupTextPlaintext(temp, msg_timestamp, s_.node_name,
                                                    reinterpret_cast<const char*>(&cmd[7]), text_len);
    proto::Packet pkt;
    if (proto::buildGroupDatagram(pkt, proto::PAYLOAD_GRP_TXT, ch.channel, temp, plen) &&
        tx_.sendGroup(pkt)) {
      ok(out);
    } else err(ERR_NOT_FOUND, out);
  }

  void sendChannelData(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 4) { err(ERR_ILLEGAL_ARG, out); return; }
    int i = 1;
    uint8_t channel_idx = cmd[i++];
    uint8_t path_len = cmd[i++];
    if (!proto::Packet::isValidPathLen(path_len) && path_len != OUT_PATH_UNKNOWN) {
      err(ERR_ILLEGAL_ARG, out); return;
    }
    uint8_t path[proto::MAX_PATH_SIZE];
    if (path_len != OUT_PATH_UNKNOWN) {
      uint8_t bl = uint8_t((path_len & 63) * ((path_len >> 6) + 1));
      std::memcpy(path, &cmd[i], bl); i += bl;
    }
    if (size_t(i) + 2 > len) { err(ERR_ILLEGAL_ARG, out); return; }
    uint16_t data_type = proto::getU16LE(cmd, i); i += 2;
    const uint8_t* payload = &cmd[i];
    int payload_len = len > size_t(i) ? int(len - i) : 0;
    ChannelDetails ch;
    if (!s_.getChannel(channel_idx, ch)) { err(ERR_NOT_FOUND, out); return; }
    if (data_type == proto::DATA_TYPE_RESERVED) { err(ERR_ILLEGAL_ARG, out); return; }
    if (payload_len > MAX_FRAME_SIZE - 9) { err(ERR_ILLEGAL_ARG, out); return; }
    uint8_t temp[3 + MAX_FRAME_SIZE];
    temp[0] = uint8_t(data_type & 0xFF);
    temp[1] = uint8_t(data_type >> 8);
    temp[2] = uint8_t(payload_len);
    if (payload_len > 0) std::memcpy(&temp[3], payload, payload_len);
    proto::Packet pkt;
    if (!proto::buildGroupDatagram(pkt, proto::PAYLOAD_GRP_DATA, ch.channel, temp, 3 + payload_len)) {
      err(ERR_TABLE_FULL, out); return;
    }
    bool okr = (path_len == OUT_PATH_UNKNOWN) ? tx_.sendGroup(pkt) : tx_.sendDirect(pkt, path, path_len);
    if (okr) ok(out); else err(ERR_TABLE_FULL, out);
  }

  void syncNextMessage(FrameWriter& out) {
    uint8_t f[MAX_FRAME_SIZE];
    int n = s_.popOffline(f);
    if (n > 0) out.writeFrame(f, n);
    else { uint8_t nm[1] = {RESP_CODE_NO_MORE_MESSAGES}; out.writeFrame(nm, 1); }
  }

  // --- Requests -----------------------------------------------------------
  // Build a PAYLOAD_REQ datagram: tag(4) || req_data, encrypted to the recipient.
  void sendReqDatagram(ContactInfo& c, const uint8_t* req_data, size_t data_len,
                       uint32_t& tag, FrameWriter& out) {
    uint8_t secret[proto::PUB_KEY_SIZE];
    s_.self.calcSharedSecret(secret, c.id);
    uint8_t temp[proto::MAX_PACKET_PAYLOAD];
    tag = tx_.rtcNowUnique();
    proto::putU32LE(temp, 0, tag);
    std::memcpy(&temp[4], req_data, data_len);
    proto::Packet pkt;
    if (!proto::buildDatagram(pkt, proto::PAYLOAD_REQ, c.id, s_.self, secret, temp, 4 + data_len)) {
      err(ERR_TABLE_FULL, out); return;
    }
    uint32_t est_timeout = 0;
    int outcome = tx_.sendToContact(pkt, c, est_timeout);
    if (outcome == SEND_FAILED) { err(ERR_TABLE_FULL, out); return; }
    sent(outcome, tag, est_timeout, out);
  }

  void sendLogin(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 1 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    ContactInfo* c = s_.lookupContact(&cmd[1], proto::PUB_KEY_SIZE);
    if (!c) { err(ERR_NOT_FOUND, out); return; }
    char password[32];
    size_t plen = len - (1 + proto::PUB_KEY_SIZE);
    if (plen > 15) plen = 15;
    std::memcpy(password, &cmd[1 + proto::PUB_KEY_SIZE], plen); password[plen] = 0;

    uint8_t secret[proto::PUB_KEY_SIZE];
    s_.self.calcSharedSecret(secret, c->id);
    uint8_t temp[24];
    uint32_t now = tx_.rtcNowUnique();
    proto::putU32LE(temp, 0, now);
    size_t tlen;
    if (c->type == ADV_TYPE_ROOM) {
      proto::putU32LE(temp, 4, c->sync_since);
      std::memcpy(&temp[8], password, plen); tlen = 8 + plen;
    } else {
      std::memcpy(&temp[4], password, plen); tlen = 4 + plen;
    }
    proto::Packet pkt;
    if (!proto::buildAnonDatagram(pkt, s_.self, c->id, secret, temp, tlen)) {
      err(ERR_TABLE_FULL, out); return;
    }
    uint32_t est_timeout = 0;
    int outcome = tx_.sendToContact(pkt, *c, est_timeout);
    if (outcome == SEND_FAILED) { err(ERR_TABLE_FULL, out); return; }
    uint32_t login_tag = proto::getU32LE(c->id.pub_key, 0);  // legacy matching scheme
    s_.pending_login = login_tag;  // match the repeater's PAYLOAD_RESPONSE reply
    sent(outcome, login_tag, est_timeout, out);
  }

  void sendAnonReq(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len <= 1 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    ContactInfo* c = s_.lookupContact(&cmd[1], proto::PUB_KEY_SIZE);
    ContactInfo anon;
    if (!c) {  // ver 13+: allow non-contact requests via a transient contact
      std::memcpy(anon.id.pub_key, &cmd[1], proto::PUB_KEY_SIZE);
      anon.out_path_len = 0; anon.type = ADV_TYPE_NONE; anon.lastmod = h_.rtcNow();
      c = &anon;
    }
    const uint8_t* data = &cmd[1 + proto::PUB_KEY_SIZE];
    size_t data_len = len - (1 + proto::PUB_KEY_SIZE);
    uint8_t secret[proto::PUB_KEY_SIZE];
    s_.self.calcSharedSecret(secret, c->id);
    uint8_t temp[proto::MAX_PACKET_PAYLOAD];
    uint32_t tag = tx_.rtcNowUnique();
    proto::putU32LE(temp, 0, tag);
    std::memcpy(&temp[4], data, data_len);
    proto::Packet pkt;
    if (!proto::buildAnonDatagram(pkt, s_.self, c->id, secret, temp, 4 + data_len)) {
      err(ERR_TABLE_FULL, out); return;
    }
    uint32_t est_timeout = 0;
    int outcome = tx_.sendToContact(pkt, *c, est_timeout);
    if (outcome == SEND_FAILED) { err(ERR_TABLE_FULL, out); return; }
    sent(outcome, tag, est_timeout, out);
  }

  void sendStatusReq(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 1 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    ContactInfo* c = s_.lookupContact(&cmd[1], proto::PUB_KEY_SIZE);
    if (!c) { err(ERR_NOT_FOUND, out); return; }
    uint32_t tag = 0;
    sendReqType(*c, REQ_TYPE_GET_STATUS, tag, out);
    s_.pending_status = proto::getU32LE(c->id.pub_key, 0);  // match reply by pub-key prefix
  }
  void sendTelemetryReq(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 4 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    ContactInfo* c = s_.lookupContact(&cmd[4], proto::PUB_KEY_SIZE);
    if (!c) { err(ERR_NOT_FOUND, out); return; }
    uint32_t tag = 0;
    sendReqType(*c, REQ_TYPE_GET_TELEMETRY_DATA, tag, out);
    s_.pending_telemetry = tag;  // match reply by request tag
  }
  void sendBinaryReq(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 2 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    ContactInfo* c = s_.lookupContact(&cmd[1], proto::PUB_KEY_SIZE);
    if (!c) { err(ERR_NOT_FOUND, out); return; }
    uint32_t tag = 0;
    sendReqDatagram(*c, &cmd[1 + proto::PUB_KEY_SIZE], len - (1 + proto::PUB_KEY_SIZE), tag, out);
    s_.pending_req = tag;  // match reply by request tag
  }
  void sendPathDiscovery(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 2 + proto::PUB_KEY_SIZE || cmd[1] != 0) { err(ERR_ILLEGAL_ARG, out); return; }
    ContactInfo* c = s_.lookupContact(&cmd[2], proto::PUB_KEY_SIZE);
    if (!c) { err(ERR_NOT_FOUND, out); return; }
    uint8_t req_data[9];
    req_data[0] = REQ_TYPE_GET_TELEMETRY_DATA;
    req_data[1] = uint8_t(~TELEM_PERM_BASE);
    std::memset(&req_data[2], 0, 3);
    proto::putU32LE(req_data, 5, tx_.random32());
    uint8_t save = c->out_path_len;
    c->out_path_len = OUT_PATH_UNKNOWN;  // force flood
    uint32_t tag = 0;
    sendReqDatagram(*c, req_data, sizeof(req_data), tag, out);
    c->out_path_len = save;
  }
  // sendReqType builds the single-byte request form (req_type + reserved + rand).
  void sendReqType(ContactInfo& c, uint8_t req_type, uint32_t& tag, FrameWriter& out) {
    uint8_t secret[proto::PUB_KEY_SIZE];
    s_.self.calcSharedSecret(secret, c.id);
    uint8_t temp[13];
    tag = tx_.rtcNowUnique();
    proto::putU32LE(temp, 0, tag);
    temp[4] = req_type;
    std::memset(&temp[5], 0, 4);
    proto::putU32LE(temp, 9, tx_.random32());
    proto::Packet pkt;
    if (!proto::buildDatagram(pkt, proto::PAYLOAD_REQ, c.id, s_.self, secret, temp, sizeof(temp))) {
      err(ERR_TABLE_FULL, out); return;
    }
    uint32_t est_timeout = 0;
    int outcome = tx_.sendToContact(pkt, c, est_timeout);
    if (outcome == SEND_FAILED) { err(ERR_TABLE_FULL, out); return; }
    sent(outcome, tag, est_timeout, out);
  }

  void hasConnectionCmd(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 1 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    if (tx_.hasConnection(&cmd[1])) ok(out); else err(ERR_NOT_FOUND, out);
  }
  void logout(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 1 + proto::PUB_KEY_SIZE) { err(ERR_ILLEGAL_ARG, out); return; }
    tx_.stopConnection(&cmd[1]); ok(out);
  }

  // --- Raw / trace / control ---------------------------------------------
  void sendRawData(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 6) { err(ERR_ILLEGAL_ARG, out); return; }
    int i = 1;
    int8_t path_len = int8_t(cmd[i++]);
    if (path_len >= 0 && size_t(i) + path_len + 4 <= len) {
      const uint8_t* path = &cmd[i];
      i += path_len;
      proto::Packet pkt;
      pkt.header = uint8_t(proto::PAYLOAD_RAW_CUSTOM << proto::PH_TYPE_SHIFT);
      size_t dlen = len - i;
      std::memcpy(pkt.payload, &cmd[i], dlen);
      pkt.payload_len = uint16_t(dlen);
      if (tx_.sendDirect(pkt, path, uint8_t(path_len))) ok(out); else err(ERR_TABLE_FULL, out);
    } else err(ERR_UNSUPPORTED_CMD, out);
  }
  void sendRawPacketCmd(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 4) { err(ERR_ILLEGAL_ARG, out); return; }
    uint8_t priority = cmd[1];
    if (tx_.sendRawPacket(&cmd[2], len - 2, priority)) ok(out); else err(ERR_ILLEGAL_ARG, out);
  }
  void sendTracePath(const uint8_t* cmd, size_t len, FrameWriter& out) {
    // cmd: code, tag(4), auth(4), flags(1), routed hashes[...]
    if (len <= 10 || len - 10 > proto::MAX_PACKET_PAYLOAD - 9) { err(ERR_ILLEGAL_ARG, out); return; }
    uint8_t path_len = uint8_t(len - 10);  // bytes of routed node hashes
    uint8_t flags = cmd[9];
    uint8_t path_sz = flags & 0x03;
    if ((path_len >> path_sz) > proto::MAX_PATH_SIZE || (path_len % (1u << path_sz)) != 0) {
      err(ERR_ILLEGAL_ARG, out); return;
    }
    uint32_t tag = proto::getU32LE(cmd, 1);
    uint32_t auth = proto::getU32LE(cmd, 5);
    proto::Packet pkt;
    pkt.header = uint8_t(proto::PAYLOAD_TRACE << proto::PH_TYPE_SHIFT);
    size_t p = 0;
    p = proto::putU32LE(pkt.payload, p, tag);
    p = proto::putU32LE(pkt.payload, p, auth);
    pkt.payload[p++] = flags;
    // TRACE is special: the routed hashes travel in the PAYLOAD (the packet's
    // path field is reserved for the per-hop SNRs each repeater appends), and the
    // routing path starts empty. See Dispatcher::handleTrace.
    std::memcpy(&pkt.payload[p], &cmd[10], path_len);
    p += path_len;
    pkt.payload_len = uint16_t(p);
    if (tx_.sendDirect(pkt, &cmd[10], 0)) {
      // Tell the app how long to wait for the round-trip, or it times out
      // instantly (est_timeout of 0). Mirrors MeshCore calcDirectTimeoutMillisFor:
      // base + (airtime*factor + per-hop) * (hops + 1) for the return through each
      // listed repeater. Airtime is the outbound frame (header+path_len byte+payload).
      uint8_t hops = uint8_t(path_len >> path_sz);
      uint32_t airtime = timeOnAirMs(pkt.payload_len + 2, float(s_.bw_hz) / 1000.0f, s_.sf, s_.cr);
      uint32_t est = 500u + uint32_t(airtime * 6.0f + 250.0f) * (uint32_t(hops) + 1u);
      sent(SEND_DIRECT, tag, est, out);
    } else err(ERR_TABLE_FULL, out);
  }
  void sendControlData(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len < 2 || (cmd[1] & 0x80) == 0) { err(ERR_ILLEGAL_ARG, out); return; }
    proto::Packet pkt;
    pkt.header = uint8_t(proto::PAYLOAD_CONTROL << proto::PH_TYPE_SHIFT);
    size_t dlen = len - 1;
    std::memcpy(pkt.payload, &cmd[1], dlen);
    pkt.payload_len = uint16_t(dlen);
    if (tx_.sendZeroHop(pkt)) ok(out); else err(ERR_TABLE_FULL, out);
  }

  // --- Signing ------------------------------------------------------------
  void signStart(FrameWriter& out) {
    uint8_t f[6]; f[0] = RESP_CODE_SIGN_START; f[1] = 0;
    proto::putU32LE(f, 2, MAX_SIGN_DATA_LEN);
    out.writeFrame(f, 6);
    sign_len_ = 0; signing_ = true;
  }
  void signData(const uint8_t* cmd, size_t len, FrameWriter& out) {
    if (len <= 1) { err(ERR_ILLEGAL_ARG, out); return; }
    if (!signing_) { err(ERR_BAD_STATE, out); return; }
    size_t n = len - 1;
    if (sign_len_ + n > sizeof(sign_buf_)) { err(ERR_TABLE_FULL, out); return; }
    std::memcpy(&sign_buf_[sign_len_], &cmd[1], n); sign_len_ += n;
    ok(out);
  }
  void signFinish(FrameWriter& out) {
    if (!signing_) { err(ERR_BAD_STATE, out); return; }
    uint8_t f[1 + proto::SIGNATURE_SIZE]; f[0] = RESP_CODE_SIGNATURE;
    s_.self.sign(&f[1], sign_buf_, sign_len_);
    signing_ = false;
    out.writeFrame(f, 1 + proto::SIGNATURE_SIZE);
  }

  // --- Small helpers ------------------------------------------------------
  static void strzcpy(char* dst, const char* src, size_t max) {
    size_t i = 0;
    for (; src[i] && i < max - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
  }
  static size_t strnlen_(const char* s, size_t max) {
    size_t i = 0; while (i < max && s[i]) i++; return i;
  }

  CompanionState& s_;
  CompanionHost& h_;
  MeshSender& tx_;

  // Signing session (bounded on the host; the reference caps at 8 KiB).
  bool signing_ = false;
  size_t sign_len_ = 0;
  uint8_t sign_buf_[2048];
};

}  // namespace corefw::companion
