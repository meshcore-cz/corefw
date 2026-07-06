// MeshService — the stable messaging surface modules use.
//
// This is the guarded boundary: modules ask the mesh to send a packet, and the
// kernel decides when/whether transmission is safe (airtime, duty cycle,
// scheduling). Modules cannot call the radio directly.
#pragma once

#include <corefw/protocol/Packet.h>

namespace corefw {

// Handler invoked by the kernel when a packet is received. Return true if the
// packet was consumed by this handler.
class PacketSink {
 public:
  virtual ~PacketSink() = default;
  virtual bool onPacket(const proto::Packet& pkt) = 0;
};

class MeshService {
 public:
  virtual ~MeshService() = default;

  // Enqueue a packet for transmission. The kernel owns scheduling; this never
  // transmits synchronously.
  virtual void send(const proto::Packet& pkt) = 0;

  // Subscribe to received packets.
  virtual void subscribe(PacketSink* sink) = 0;

  // The local node's public-key prefix (PATH_HASH_SIZE bytes), for addressing.
  virtual const uint8_t* selfHash() const = 0;
};

}  // namespace corefw
