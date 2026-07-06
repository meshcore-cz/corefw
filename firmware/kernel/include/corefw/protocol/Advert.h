// Advert packet construction & verification.
//
// Assembles/validates a PAYLOAD_TYPE_ADVERT packet byte-identically to the
// reference firmware (Mesh::createAdvert / the ADVERT branch of onRecvPacket):
//
//   payload = pub_key(32) || timestamp(LE32) || signature(64) || app_data
//   signed  = pub_key(32) || timestamp(LE32) || app_data
#pragma once

#include <corefw/protocol/AdvertData.h>
#include <corefw/protocol/Identity.h>
#include <corefw/protocol/Packet.h>

namespace corefw::proto {

// Offsets within an advert payload.
inline constexpr size_t ADVERT_OFS_TIMESTAMP = PUB_KEY_SIZE;
inline constexpr size_t ADVERT_OFS_SIGNATURE = PUB_KEY_SIZE + 4;
inline constexpr size_t ADVERT_OFS_APPDATA = PUB_KEY_SIZE + 4 + SIGNATURE_SIZE;

// buildAdvert fills `pkt` with a signed advert. The route type is left as
// ROUTE_FLOOD (set by the caller/router as appropriate). Returns false if the
// app data is too large.
inline bool buildAdvert(Packet& pkt, const LocalIdentity& id, uint32_t timestamp,
                        const uint8_t* app_data, uint8_t app_data_len) {
  if (app_data_len > MAX_ADVERT_DATA_SIZE) return false;

  pkt.setRouteAndType(ROUTE_FLOOD, PAYLOAD_ADVERT);

  size_t len = 0;
  std::memcpy(&pkt.payload[len], id.pub_key, PUB_KEY_SIZE);
  len += PUB_KEY_SIZE;
  len = putU32LE(pkt.payload, len, timestamp);

  uint8_t* signature = &pkt.payload[len];
  len += SIGNATURE_SIZE;

  std::memcpy(&pkt.payload[len], app_data, app_data_len);
  len += app_data_len;
  pkt.payload_len = uint16_t(len);

  // Sign pub_key || timestamp || app_data.
  uint8_t msg[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE];
  size_t msg_len = buildAdvertSignedMessage(msg, id.pub_key, timestamp, app_data, app_data_len);
  id.sign(signature, msg, msg_len);
  return true;
}

// Convenience overload taking a structured AdvertData.
inline bool buildAdvert(Packet& pkt, const LocalIdentity& id, uint32_t timestamp,
                        const AdvertData& data) {
  uint8_t app[MAX_ADVERT_DATA_SIZE];
  uint8_t n = data.encode(app);
  return buildAdvert(pkt, id, timestamp, app, n);
}

// parseAdvert extracts the identity, timestamp and app data from an advert
// payload and verifies the signature. Returns false on truncation or a forged
// signature (matching the reference firmware, which drops such packets).
inline bool parseAdvert(const Packet& pkt, Identity& out_id, uint32_t& out_ts,
                        AdvertData& out_data) {
  if (pkt.payloadType() != PAYLOAD_ADVERT) return false;
  if (pkt.payload_len < ADVERT_OFS_APPDATA) return false;

  std::memcpy(out_id.pub_key, &pkt.payload[0], PUB_KEY_SIZE);
  out_ts = getU32LE(pkt.payload, ADVERT_OFS_TIMESTAMP);
  const uint8_t* signature = &pkt.payload[ADVERT_OFS_SIGNATURE];

  uint8_t app_data_len = uint8_t(pkt.payload_len - ADVERT_OFS_APPDATA);
  if (app_data_len > MAX_ADVERT_DATA_SIZE) app_data_len = MAX_ADVERT_DATA_SIZE;
  const uint8_t* app_data = &pkt.payload[ADVERT_OFS_APPDATA];

  uint8_t msg[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE];
  size_t msg_len = buildAdvertSignedMessage(msg, out_id.pub_key, out_ts, app_data, app_data_len);
  if (!out_id.verify(signature, msg, msg_len)) return false;

  return out_data.decode(app_data, app_data_len);
}

}  // namespace corefw::proto
