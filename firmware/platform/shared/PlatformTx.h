// Shared transmit helpers for the per-architecture target mains.
//
// Route stamping, return-path attachment and self-advert finalisation are
// identical on every board, so they live here once instead of being copy-pasted
// into esp32_target_main.cpp, nrf52_target_main.cpp and every future port. The
// mains bring these into scope with `using namespace corefw::platform;`.
//
// Target-only: pulled in behind COREFW_TARGET by the platform mains. It touches
// only kernel/protocol types (no Arduino/board headers), so it stays portable
// across architectures.
#pragma once

#include <corefw/Kernel.h>
#include <corefw/protocol/Advert.h>
#include <corefw/protocol/Identity.h>
#include <corefw/protocol/Packet.h>
#include <corefw/runtime/Dispatcher.h>

#include <cstring>

namespace corefw::platform {

// Set the route bits on a packet, preserving the payload type. Datagram
// builders leave route = 0; the sender stamps FLOOD or DIRECT here.
inline void setRoute(proto::Packet& pkt, uint8_t route) {
  pkt.header = uint8_t((pkt.header & ~proto::PH_ROUTE_MASK) | (route & proto::PH_ROUTE_MASK));
}

// Attach a return path (capped at 63 hops). The caller sets ROUTE_DIRECT.
inline void applyPath(proto::Packet& pkt, const uint8_t* path, uint8_t path_len) {
  uint8_t n = path_len & 63;
  pkt.setPathHashSizeAndCount(1, n);
  std::memcpy(pkt.path, path, n);
}

// Finalise and send an advert whose type/name/location are already populated:
// let every registered extension enrich it (Kernel::applyAdvertDecorators),
// sign+build it, stamp the flood route (zero-hop when flood is false) and hand
// it to the scheduler. Returns false only if the advert app-data is too large.
inline bool emitAdvert(Dispatcher& disp, Kernel& kernel,
                       const proto::LocalIdentity& id, uint32_t now,
                       proto::AdvertData& ad, bool flood) {
  kernel.applyAdvertDecorators(ad);
  proto::Packet pkt;
  if (!proto::buildAdvert(pkt, id, now, ad)) return false;
  setRoute(pkt, proto::ROUTE_FLOOD);
  if (!flood) pkt.setPathHashSizeAndCount(1, 0);  // zero-hop
  disp.send(pkt);
  return true;
}

}  // namespace corefw::platform
