// Companion Protocol constants.
//
// The Companion Protocol is the framed request/response/push channel between a
// node and a phone/app over serial (USB), BLE or TCP. These command, response
// and push codes match the reference companion firmware
// (examples/companion_radio/MyMesh.cpp) exactly, so existing MeshCore apps talk
// to a corefw companion unchanged.
#pragma once

#include <cstdint>

namespace corefw::companion {

// Frame start markers (serial/USB transport). Outbound frames (device -> app)
// begin with '>'; inbound frames (app -> device) begin with '<'. Both are
// followed by a little-endian uint16 length then the payload.
inline constexpr uint8_t FRAME_START_OUT = '>';  // 0x3E, device -> app
inline constexpr uint8_t FRAME_START_IN = '<';   // 0x3C, app -> device

// App -> device command codes (payload byte 0).
enum Cmd : uint8_t {
  CMD_APP_START = 1,
  CMD_SEND_TXT_MSG = 2,
  CMD_SEND_CHANNEL_TXT_MSG = 3,
  CMD_GET_CONTACTS = 4,
  CMD_GET_DEVICE_TIME = 5,
  CMD_SET_DEVICE_TIME = 6,
  CMD_SEND_SELF_ADVERT = 7,
  CMD_SET_ADVERT_NAME = 8,
  CMD_ADD_UPDATE_CONTACT = 9,
  CMD_SYNC_NEXT_MESSAGE = 10,
  CMD_SET_RADIO_PARAMS = 11,
  CMD_SET_RADIO_TX_POWER = 12,
  CMD_RESET_PATH = 13,
  CMD_SET_ADVERT_LATLON = 14,
  CMD_REMOVE_CONTACT = 15,
  CMD_SHARE_CONTACT = 16,
  CMD_EXPORT_CONTACT = 17,
  CMD_IMPORT_CONTACT = 18,
  CMD_REBOOT = 19,
  CMD_GET_BATT_AND_STORAGE = 20,
  CMD_SET_TUNING_PARAMS = 21,
  CMD_DEVICE_QUERY = 22,
  CMD_EXPORT_PRIVATE_KEY = 23,
  CMD_IMPORT_PRIVATE_KEY = 24,
  CMD_SEND_RAW_DATA = 25,
  CMD_SEND_LOGIN = 26,
  CMD_SEND_STATUS_REQ = 27,
  CMD_HAS_CONNECTION = 28,
  CMD_LOGOUT = 29,
  CMD_GET_CONTACT_BY_KEY = 30,
  CMD_GET_CHANNEL = 31,
  CMD_SET_CHANNEL = 32,
  CMD_SIGN_START = 33,
  CMD_SIGN_DATA = 34,
  CMD_SIGN_FINISH = 35,
  CMD_SEND_TRACE_PATH = 36,
  CMD_SET_DEVICE_PIN = 37,
  CMD_SET_OTHER_PARAMS = 38,
  CMD_SEND_TELEMETRY_REQ = 39,
  CMD_GET_CUSTOM_VARS = 40,
  CMD_SET_CUSTOM_VAR = 41,
  CMD_GET_ADVERT_PATH = 42,
  CMD_GET_TUNING_PARAMS = 43,
  CMD_DEVICE_QUERY_EXT = 44,
};

// Device -> app response codes (payload byte 0), sent in reply to a command.
enum Resp : uint8_t {
  RESP_CODE_OK = 0,
  RESP_CODE_ERR = 1,
  RESP_CODE_CONTACTS_START = 2,
  RESP_CODE_CONTACT = 3,
  RESP_CODE_END_OF_CONTACTS = 4,
  RESP_CODE_SELF_INFO = 5,
  RESP_CODE_SENT = 6,
  RESP_CODE_CONTACT_MSG_RECV = 7,
  RESP_CODE_CHANNEL_MSG_RECV = 8,
  RESP_CODE_CURR_TIME = 9,
  RESP_CODE_NO_MORE_MESSAGES = 10,
  RESP_CODE_EXPORT_CONTACT = 11,
  RESP_CODE_BATT_AND_STORAGE = 12,
  RESP_CODE_DEVICE_INFO = 13,
  RESP_CODE_PRIVATE_KEY = 14,
  RESP_CODE_DISABLED = 15,
  RESP_CODE_CONTACT_MSG_RECV_V3 = 16,
  RESP_CODE_CHANNEL_MSG_RECV_V3 = 17,
  RESP_CODE_CHANNEL_INFO = 18,
  RESP_CODE_SIGN_START = 19,
  RESP_CODE_SIGNATURE = 20,
  RESP_CODE_CUSTOM_VARS = 21,
  RESP_CODE_ADVERT_PATH = 22,
  RESP_CODE_TUNING_PARAMS = 23,
  RESP_CODE_STATS = 24,
  RESP_CODE_AUTOADD_CONFIG = 25,
  RESP_CODE_CHANNEL_DATA_RECV = 27,
  RESP_CODE_DEFAULT_FLOOD_SCOPE = 28,
};

// Device -> app push codes (payload byte 0), sent unsolicited. All have the top
// bit set, which disambiguates them from response codes.
enum Push : uint8_t {
  PUSH_CODE_ADVERT = 0x80,
  PUSH_CODE_PATH_UPDATED = 0x81,
  PUSH_CODE_SEND_CONFIRMED = 0x82,
  PUSH_CODE_MSG_WAITING = 0x83,
  PUSH_CODE_RAW_DATA = 0x84,
  PUSH_CODE_LOGIN_SUCCESS = 0x85,
  PUSH_CODE_LOGIN_FAIL = 0x86,
  PUSH_CODE_STATUS_RESPONSE = 0x87,
  PUSH_CODE_LOG_RX_DATA = 0x88,
  PUSH_CODE_TRACE_DATA = 0x89,
  PUSH_CODE_NEW_ADVERT = 0x8A,
  PUSH_CODE_TELEMETRY_RESPONSE = 0x8B,
  PUSH_CODE_BINARY_RESPONSE = 0x8C,
  PUSH_CODE_PATH_DISCOVERY_RESPONSE = 0x8D,
  PUSH_CODE_CONTROL_DATA = 0x8E,
  PUSH_CODE_CONTACT_DELETED = 0x8F,
  PUSH_CODE_CONTACTS_FULL = 0x90,
};

// Maximum companion frame payload (matches BaseSerialInterface.h: 176, which is
// the mesh payload + 4 for transport codes / region scoping).
inline constexpr int MAX_FRAME_SIZE = 176;

}  // namespace corefw::companion
