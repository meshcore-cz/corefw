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

// Observes every raw frame received off the radio, before parsing — the kernel
// hook that realises MeshCore's Dispatcher::logRxRaw. Diagnostic only (it never
// mutates the frame): the companion role uses it to stream LOG_RX_DATA to the
// app's packet-log view, over whatever transport is attached.
class RawRxObserver {
 public:
  virtual ~RawRxObserver() = default;
  virtual void onRawRx(const uint8_t* raw, size_t len, int8_t snr_q4, int8_t rssi) = 0;
};

// Observes a TRACE packet that has reached the end of its routed path at this
// node — i.e. we are the trace originator hearing the final hop echo back. The
// companion role turns this into a PUSH_CODE_TRACE_DATA frame for the app.
// Mirrors MeshCore's Mesh::onTraceRecv. `snrs` holds one q4 RX-SNR per traversed
// hop (snr_count of them); `hashes` is the routed node-hash list (hash_len
// bytes); `final_snr_q4` is our own RX SNR of the returning packet.
class TraceObserver {
 public:
  virtual ~TraceObserver() = default;
  virtual void onTrace(uint32_t tag, uint32_t auth, uint8_t flags,
                       const uint8_t* snrs, uint8_t snr_count,
                       const uint8_t* hashes, uint8_t hash_len,
                       int8_t final_snr_q4) = 0;
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
