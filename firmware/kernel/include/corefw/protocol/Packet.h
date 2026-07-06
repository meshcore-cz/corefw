// Packet — the fundamental Core Protocol transmission unit.
//
// This is a clean-room reimplementation of the MeshCore V1 packet wire format.
// It is intentionally identical on the wire to the reference firmware
// (src/Packet.cpp) so corefw nodes interoperate on the same mesh. The header
// bit layout, path-length encoding and transport-code placement all match.
#pragma once

#include <corefw/protocol/Wire.h>

namespace corefw::proto {

// Header bit fields (matches PH_* in the reference firmware).
inline constexpr uint8_t PH_ROUTE_MASK = 0x03;  // bits 0-1
inline constexpr uint8_t PH_TYPE_SHIFT = 2;
inline constexpr uint8_t PH_TYPE_MASK = 0x0F;  // bits 2-5
inline constexpr uint8_t PH_VER_SHIFT = 6;
inline constexpr uint8_t PH_VER_MASK = 0x03;  // bits 6-7

// Route types.
enum RouteType : uint8_t {
  ROUTE_TRANSPORT_FLOOD = 0x00,
  ROUTE_FLOOD = 0x01,
  ROUTE_DIRECT = 0x02,
  ROUTE_TRANSPORT_DIRECT = 0x03,
};

// Payload types.
enum PayloadType : uint8_t {
  PAYLOAD_REQ = 0x00,
  PAYLOAD_RESPONSE = 0x01,
  PAYLOAD_TXT_MSG = 0x02,
  PAYLOAD_ACK = 0x03,
  PAYLOAD_ADVERT = 0x04,
  PAYLOAD_GRP_TXT = 0x05,
  PAYLOAD_GRP_DATA = 0x06,
  PAYLOAD_ANON_REQ = 0x07,
  PAYLOAD_PATH = 0x08,
  PAYLOAD_TRACE = 0x09,
  PAYLOAD_MULTIPART = 0x0A,
  PAYLOAD_CONTROL = 0x0B,
  PAYLOAD_RAW_CUSTOM = 0x0F,
};

// Payload version (V1 => 0).
inline constexpr uint8_t PAYLOAD_VER_1 = 0x00;

class Packet {
 public:
  uint8_t header = 0;
  uint16_t payload_len = 0;
  uint16_t path_len = 0;  // encoded: low 6 bits = hash count, top 2 bits = hash size-1
  uint16_t transport_codes[2] = {0, 0};
  uint8_t path[MAX_PATH_SIZE] = {};
  uint8_t payload[MAX_PACKET_PAYLOAD] = {};
  int8_t snr = 0;

  // --- Header accessors (bit-identical to the reference firmware) ---------
  uint8_t routeType() const { return header & PH_ROUTE_MASK; }
  uint8_t payloadType() const { return (header >> PH_TYPE_SHIFT) & PH_TYPE_MASK; }
  uint8_t payloadVer() const { return (header >> PH_VER_SHIFT) & PH_VER_MASK; }

  bool isRouteFlood() const {
    return routeType() == ROUTE_FLOOD || routeType() == ROUTE_TRANSPORT_FLOOD;
  }
  bool isRouteDirect() const {
    return routeType() == ROUTE_DIRECT || routeType() == ROUTE_TRANSPORT_DIRECT;
  }
  bool hasTransportCodes() const {
    return routeType() == ROUTE_TRANSPORT_FLOOD || routeType() == ROUTE_TRANSPORT_DIRECT;
  }

  // --- Path encoding ------------------------------------------------------
  uint8_t pathHashSize() const { return uint8_t((path_len >> 6) + 1); }
  uint8_t pathHashCount() const { return uint8_t(path_len & 63); }
  uint8_t pathByteLen() const { return uint8_t(pathHashCount() * pathHashSize()); }
  void setPathHashSizeAndCount(uint8_t sz, uint8_t n) {
    path_len = uint16_t(((sz - 1) << 6) | (n & 63));
  }
  // Set just the hash count, preserving the encoded hash size.
  void setPathHashCount(uint8_t n) {
    path_len = uint16_t((path_len & ~uint16_t(63)) | (n & 63));
  }

  // Mark a packet so routers will not retransmit it (header sentinel 0xFF).
  void markDoNotRetransmit() { header = 0xFF; }
  bool isMarkedDoNotRetransmit() const { return header == 0xFF; }

  // SNR of the last reception, in dB (stored as quarter-dB in `snr`).
  float snrDb() const { return float(snr) / 4.0f; }

  static bool isValidPathLen(uint8_t pl) {
    uint8_t hash_count = pl & 63;
    uint8_t hash_size = uint8_t((pl >> 6) + 1);
    if (hash_size == 4) return false;  // reserved
    return size_t(hash_count) * hash_size <= MAX_PATH_SIZE;
  }

  void setRouteAndType(uint8_t route, uint8_t type, uint8_t ver = PAYLOAD_VER_1) {
    header = uint8_t((route & PH_ROUTE_MASK) | ((type & PH_TYPE_MASK) << PH_TYPE_SHIFT) |
                     ((ver & PH_VER_MASK) << PH_VER_SHIFT));
  }

  // getRawLength returns the encoded wire length.
  size_t rawLength() const {
    return 2 + pathByteLen() + payload_len + (hasTransportCodes() ? 4 : 0);
  }

  // writeTo serialises the packet, returning the number of bytes written.
  size_t writeTo(uint8_t* dst) const {
    size_t i = 0;
    dst[i++] = header;
    if (hasTransportCodes()) {
      i = putU16LE(dst, i, transport_codes[0]);
      i = putU16LE(dst, i, transport_codes[1]);
    }
    dst[i++] = uint8_t(path_len);
    uint8_t bl = pathByteLen();
    std::memcpy(&dst[i], path, bl);
    i += bl;
    std::memcpy(&dst[i], payload, payload_len);
    i += payload_len;
    return i;
  }

  // readFrom parses a packet from a wire blob. Returns false on bad encoding.
  bool readFrom(const uint8_t* src, size_t len) {
    size_t i = 0;
    if (len < 2) return false;
    header = src[i++];
    if (hasTransportCodes()) {
      if (i + 4 > len) return false;
      transport_codes[0] = getU16LE(src, i); i += 2;
      transport_codes[1] = getU16LE(src, i); i += 2;
    } else {
      transport_codes[0] = transport_codes[1] = 0;
    }
    if (i >= len) return false;
    path_len = src[i++];
    if (!isValidPathLen(uint8_t(path_len))) return false;
    uint8_t bl = pathByteLen();
    if (i + bl > len) return false;
    std::memcpy(path, &src[i], bl); i += bl;
    if (i > len) return false;
    payload_len = uint16_t(len - i);
    if (payload_len > sizeof(payload)) return false;
    std::memcpy(payload, &src[i], payload_len);
    return true;
  }
};

}  // namespace corefw::proto
