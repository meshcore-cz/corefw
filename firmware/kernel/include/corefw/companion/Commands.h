// Companion Protocol command handling.
//
// Parses inbound app->device command frames and produces byte-identical
// device->app response frames, matching the reference companion firmware
// (examples/companion_radio/MyMesh.cpp handleCmdFrame). Device state and side
// effects (RTC, battery, storage, sending adverts) are reached through the
// CompanionHost interface, so this whole layer is portable and host-testable —
// the frames it emits are exactly what existing MeshCore apps expect.
#pragma once

#include <corefw/companion/Protocol.h>
#include <corefw/protocol/Wire.h>

#include <cstring>

namespace corefw::companion {

// Protocol version reported to the app. This is the compatibility knob apps key
// their behaviour on; it must track the MeshCore companion protocol version.
inline constexpr uint8_t FIRMWARE_VER_CODE = 13;
inline constexpr const char* FIRMWARE_VERSION = "corefw-0.1.0";
inline constexpr const char* FIRMWARE_BUILD_DATE = "2026";

// Advert node type reported in SELF_INFO (a companion identifies as CHAT).
inline constexpr uint8_t ADV_TYPE_CHAT = 1;

// Advert location policies.
inline constexpr uint8_t ADVERT_LOC_NONE = 0;
inline constexpr uint8_t ADVERT_LOC_SHARE = 1;

// Error codes (second byte of a RESP_CODE_ERR frame).
enum Err : uint8_t {
  ERR_UNSUPPORTED_CMD = 1,
  ERR_NOT_FOUND = 2,
  ERR_TABLE_FULL = 3,
  ERR_BAD_STATE = 4,
  ERR_FILE_IO = 5,
  ERR_ILLEGAL_ARG = 6,
};

// CompanionState is the device configuration the handler reads and mutates.
// Radio units mirror the reference SELF_INFO frame: freq in kHz, bandwidth in Hz.
struct CompanionState {
  uint8_t self_pub[proto::PUB_KEY_SIZE] = {};
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
  uint8_t telemetry_flags = 0;
  uint8_t manual_add_contacts = 0;
  uint8_t client_repeat = 0;
  uint8_t path_hash_mode = 0;
  uint8_t max_contacts = 100;
  uint8_t max_group_channels = 40;
};

// CompanionHost bridges the handler to device services.
class CompanionHost {
 public:
  virtual ~CompanionHost() = default;
  virtual uint32_t rtcNow() = 0;                 // UNIX seconds
  virtual void setRtc(uint32_t secs) = 0;
  virtual uint16_t batteryMilliVolts() = 0;
  virtual uint32_t storageUsedKb() { return 0; }
  virtual uint32_t storageTotalKb() { return 0; }
  virtual void sendSelfAdvert(bool flood) = 0;   // emit a self-advert on the mesh
  virtual const char* manufacturerName() { return "corefw"; }
  virtual void savePrefs() {}
};

// CommandHandler processes decoded inbound frames.
class CommandHandler {
 public:
  CommandHandler(CompanionState& state, CompanionHost& host) : s_(state), h_(host) {}

  // handle processes one inbound command payload (cmd[0] is the code). It writes
  // the response payload into `out` (capacity MAX_FRAME_SIZE) and returns its
  // length, or 0 when there is no reply. Unknown commands yield an ERR frame.
  size_t handle(const uint8_t* cmd, size_t len, uint8_t* out) {
    if (len == 0) return 0;
    switch (cmd[0]) {
      case CMD_DEVICE_QUERY:
        return len >= 2 ? deviceInfo(cmd[1], out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_APP_START:
        return len >= 8 ? selfInfo(out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_GET_DEVICE_TIME:
        return currTime(out);
      case CMD_SET_DEVICE_TIME:
        return len >= 5 ? setTime(cmd, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_SET_ADVERT_NAME:
        return len >= 2 ? setName(cmd, len, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_SET_ADVERT_LATLON:
        return len >= 9 ? setLatLon(cmd, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_SET_RADIO_TX_POWER:
        return len >= 2 ? setTxPower(cmd, out) : err(ERR_ILLEGAL_ARG, out);
      case CMD_SEND_SELF_ADVERT:
        h_.sendSelfAdvert(len >= 2 && cmd[1] == 1);
        return ok(out);
      case CMD_GET_BATT_AND_STORAGE:
        return battAndStorage(out);
      case CMD_REBOOT:
        return ok(out);  // the caller performs the actual reboot
      default:
        return err(ERR_UNSUPPORTED_CMD, out);
    }
  }

 private:
  size_t ok(uint8_t* out) {
    out[0] = RESP_CODE_OK;
    return 1;
  }
  size_t err(uint8_t code, uint8_t* out) {
    out[0] = RESP_CODE_ERR;
    out[1] = code;
    return 2;
  }

  size_t deviceInfo(uint8_t appVer, uint8_t* out) {
    app_target_ver_ = appVer;
    size_t i = 0;
    out[i++] = RESP_CODE_DEVICE_INFO;
    out[i++] = FIRMWARE_VER_CODE;
    out[i++] = uint8_t(s_.max_contacts / 2);
    out[i++] = s_.max_group_channels;
    i = proto::putU32LE(out, i, s_.ble_pin);
    std::memset(&out[i], 0, 12);
    strzcpy(reinterpret_cast<char*>(&out[i]), FIRMWARE_BUILD_DATE, 12);
    i += 12;
    strzcpy(reinterpret_cast<char*>(&out[i]), h_.manufacturerName(), 40);
    i += 40;
    strzcpy(reinterpret_cast<char*>(&out[i]), FIRMWARE_VERSION, 20);
    i += 20;
    out[i++] = s_.client_repeat;
    out[i++] = s_.path_hash_mode;
    return i;
  }

  size_t selfInfo(uint8_t* out) {
    size_t i = 0;
    out[i++] = RESP_CODE_SELF_INFO;
    out[i++] = ADV_TYPE_CHAT;
    out[i++] = uint8_t(s_.tx_power_dbm);
    out[i++] = uint8_t(s_.max_tx_power_dbm);
    std::memcpy(&out[i], s_.self_pub, proto::PUB_KEY_SIZE);
    i += proto::PUB_KEY_SIZE;
    i = proto::putU32LE(out, i, uint32_t(s_.lat_e6));
    i = proto::putU32LE(out, i, uint32_t(s_.lon_e6));
    out[i++] = s_.multi_acks;
    out[i++] = s_.advert_loc_policy;
    out[i++] = s_.telemetry_flags;
    out[i++] = s_.manual_add_contacts;
    i = proto::putU32LE(out, i, s_.freq_khz);
    i = proto::putU32LE(out, i, s_.bw_hz);
    out[i++] = s_.sf;
    out[i++] = s_.cr;
    size_t nlen = std::strlen(s_.node_name);
    std::memcpy(&out[i], s_.node_name, nlen);
    i += nlen;
    return i;
  }

  size_t currTime(uint8_t* out) {
    out[0] = RESP_CODE_CURR_TIME;
    proto::putU32LE(out, 1, h_.rtcNow());
    return 5;
  }

  size_t setTime(const uint8_t* cmd, uint8_t* out) {
    uint32_t secs = proto::getU32LE(cmd, 1);
    if (secs >= h_.rtcNow()) {
      h_.setRtc(secs);
      return ok(out);
    }
    return err(ERR_ILLEGAL_ARG, out);
  }

  size_t setName(const uint8_t* cmd, size_t len, uint8_t* out) {
    size_t nlen = len - 1;
    if (nlen > sizeof(s_.node_name) - 1) nlen = sizeof(s_.node_name) - 1;
    std::memcpy(s_.node_name, &cmd[1], nlen);
    s_.node_name[nlen] = 0;
    h_.savePrefs();
    return ok(out);
  }

  size_t setLatLon(const uint8_t* cmd, uint8_t* out) {
    int32_t lat = int32_t(proto::getU32LE(cmd, 1));
    int32_t lon = int32_t(proto::getU32LE(cmd, 5));
    if (lat <= 90000000 && lat >= -90000000 && lon <= 180000000 && lon >= -180000000) {
      s_.lat_e6 = lat;
      s_.lon_e6 = lon;
      h_.savePrefs();
      return ok(out);
    }
    return err(ERR_ILLEGAL_ARG, out);
  }

  size_t setTxPower(const uint8_t* cmd, uint8_t* out) {
    int8_t p = int8_t(cmd[1]);
    if (p > s_.max_tx_power_dbm) p = s_.max_tx_power_dbm;
    s_.tx_power_dbm = p;
    h_.savePrefs();
    return ok(out);
  }

  size_t battAndStorage(uint8_t* out) {
    size_t i = 0;
    out[i++] = RESP_CODE_BATT_AND_STORAGE;
    uint16_t mv = h_.batteryMilliVolts();
    i = proto::putU16LE(out, i, mv);
    i = proto::putU32LE(out, i, h_.storageUsedKb());
    i = proto::putU32LE(out, i, h_.storageTotalKb());
    return i;
  }

  // strzcpy copies up to max-1 bytes and null-terminates within max (matches
  // the reference StrHelper::strzcpy).
  static void strzcpy(char* dst, const char* src, size_t max) {
    size_t i = 0;
    for (; src[i] && i < max - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
  }

  CompanionState& s_;
  CompanionHost& h_;
  uint8_t app_target_ver_ = 0;
};

}  // namespace corefw::companion
