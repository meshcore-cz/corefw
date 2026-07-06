// FloodRouter — the Core Protocol flood routing decision.
//
// Pure logic (no radio, no clock) so it can be exhaustively host-tested. Given a
// received packet, the local node's public key and the dedup table, it decides
// whether to deliver the packet locally and whether to re-broadcast it, applying
// the same rules as the reference firmware's routeRecvPacket: on a fresh flood
// packet the node appends its own hash to the path and retransmits, giving lower
// priority the further a packet has travelled.
#pragma once

#include <corefw/protocol/Packet.h>
#include <corefw/runtime/Dedup.h>

namespace corefw {

struct RouteDecision {
  bool duplicate = false;  // already seen — drop
  bool deliver = false;    // hand to local sinks
  bool forward = false;    // re-broadcast (path/count already updated)
  uint8_t priority = 0;    // retransmit priority = new path hash count
};

class FloodRouter {
 public:
  // route inspects `pkt` (mutating its path on a forward). `self_pub` is the
  // local node's 32-byte public key; only its prefix (the packet's hash size)
  // is appended to the path.
  RouteDecision route(proto::Packet& pkt, const uint8_t* self_pub, Dedup& seen) const {
    RouteDecision d;
    if (seen.checkAndMark(pkt)) {
      d.duplicate = true;
      return d;
    }
    d.deliver = true;

    if (!pkt.isRouteFlood() || pkt.isMarkedDoNotRetransmit()) {
      return d;  // direct/marked packets are not flood-forwarded here
    }
    const uint8_t n = pkt.pathHashCount();
    const uint8_t sz = pkt.pathHashSize();
    if (size_t(n + 1) * sz <= proto::MAX_PATH_SIZE) {
      // Append this node's hash prefix and bump the count.
      std::memcpy(&pkt.path[n * sz], self_pub, sz);
      pkt.setPathHashCount(uint8_t(n + 1));
      d.forward = true;
      d.priority = pkt.pathHashCount();
    }
    return d;
  }
};

}  // namespace corefw
