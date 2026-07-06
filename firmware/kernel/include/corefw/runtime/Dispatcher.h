// Dispatcher — the radio scheduler that realises MeshService.
//
// This is the guarded core: modules call send()/subscribe() and never see the
// radio. The dispatcher pumps received frames through the FloodRouter, delivers
// local packets to subscribers, queues forwards, and drains the transmit queue
// onto the radio only when the duty-cycle limiter permits. It owns *when* things
// go on air; modules own *what*.
#pragma once

#include <cstring>

#include <corefw/Mesh.h>
#include <corefw/RadioDriver.h>
#include <corefw/runtime/Airtime.h>
#include <corefw/runtime/Clock.h>
#include <corefw/runtime/Dedup.h>
#include <corefw/runtime/FloodRouter.h>

namespace corefw {

inline constexpr int kTxQueueSize = 16;
inline constexpr int kMaxSinks = 8;

class Dispatcher : public MeshService {
 public:
  Dispatcher(RadioDriver* radio, Clock* clock, const uint8_t self_pub[proto::PUB_KEY_SIZE],
             const RadioConfig& cfg)
      : radio_(radio), clock_(clock), cfg_(cfg) {
    std::memcpy(self_pub_, self_pub, proto::PUB_KEY_SIZE);
  }

  void setDuty(float fraction) { duty_.setDuty(fraction); }

  // MeshCore companion: allowPacketForward() returns client_repeat != 0. Repeaters
  // leave this enabled (default); companions disable flood rebroadcast unless the
  // app opts in via SET_RADIO_PARAMS.
  void setAllowFloodForward(bool allow) { allow_flood_forward_ = allow; }
  bool allowFloodForward() const { return allow_flood_forward_; }

  // Register a diagnostic observer of raw received frames (LOG_RX_DATA source).
  void setRawRxObserver(RawRxObserver* obs) { rx_observer_ = obs; }
  void setTraceObserver(TraceObserver* obs) { trace_observer_ = obs; }

  void configureRadio(const RadioConfig& cfg) {
    cfg_ = cfg;
    if (radio_) radio_->configure(cfg_);
  }

  // --- MeshService --------------------------------------------------------
  void send(const proto::Packet& pkt) override {
    // Record our own packet so its echoes are suppressed, then queue it.
    seen_.markSeen(pkt);
    enqueue(pkt, clock_->millis());
  }

  void subscribe(PacketSink* sink) override {
    if (sink_count_ < kMaxSinks) sinks_[sink_count_++] = sink;
  }

  const uint8_t* selfHash() const override { return self_pub_; }

  // --- Runtime pump -------------------------------------------------------
  // Call frequently from the kernel loop. Processes at most one received frame
  // and at most one transmission per call (half-duplex radio).
  void loop() {
    if (radio_ == nullptr) return;
    const uint32_t now = clock_->millis();
    radio_->loop();
    pumpReceive(now);
    pumpTransmit(now);
  }

  // Stats, useful for diagnostics and tests.
  uint32_t forwarded() const { return forwarded_; }
  uint32_t delivered() const { return delivered_; }
  uint32_t duplicates() const { return duplicates_; }
  int queueDepth() const { return tx_count_; }

 private:
  struct TxEntry {
    proto::Packet pkt;
    uint32_t ready_at;
    uint8_t priority;
    bool used;
  };

  void pumpReceive(uint32_t now) {
    uint8_t buf[proto::MAX_TRANS_UNIT];
    size_t n = radio_->readReceived(buf, sizeof(buf));
    if (n == 0) return;
    if (rx_observer_) {
      rx_observer_->onRawRx(buf, n, int8_t(radio_->lastSNR() * 4.0f),
                            int8_t(radio_->lastRSSI()));
    }
    proto::Packet pkt;
    if (!pkt.readFrom(buf, n)) return;
    pkt.snr = int8_t(radio_->lastSNR() * 4.0f);

    // TRACE packets are routed specially: the hash list lives in the payload and
    // the "path" field accumulates per-hop SNRs, so they bypass the flood router.
    if (pkt.isRouteDirect() && pkt.payloadType() == proto::PAYLOAD_TRACE) {
      handleTrace(pkt, now);
      return;
    }

    RouteDecision d = router_.route(pkt, self_pub_, seen_);
    if (d.duplicate) {
      duplicates_++;
      return;
    }
    if (d.deliver) {
      delivered_++;
      for (int i = 0; i < sink_count_; i++) sinks_[i]->onPacket(pkt);
    }
    if (d.forward && allow_flood_forward_) {
      const uint32_t airtime = txAirtime(pkt);
      // Further-travelled packets wait longer, yielding priority to nodes
      // closer to the source (matches the reference firmware's intent).
      enqueue(pkt, now + retransmitDelay(d.priority, airtime), d.priority);
    }
  }

  // Handle a received TRACE packet (MeshCore Mesh::onRecvPacket TRACE branch).
  // Payload = tag(4) ‖ auth(4) ‖ flags(1) ‖ hashes[...]; pkt.path accumulates one
  // SNR (q4) per hop and pathHashCount() is that running count (hash size is 1 so
  // the encoded path_len equals the raw SNR count).
  void handleTrace(proto::Packet& pkt, uint32_t now) {
    if (pkt.payload_len < 9) return;
    const uint8_t path_sz = pkt.payload[8] & 0x03;
    const uint8_t hash_len = uint8_t(pkt.payload_len - 9);   // bytes of routed hashes
    const uint8_t snr_count = pkt.pathHashCount();           // hops traversed so far
    const uint16_t offset = uint16_t(snr_count) << path_sz;  // hash bytes consumed

    if (offset >= hash_len) {
      // Reached the end of the given path: we are the originator hearing the
      // final hop. Surface the collected trace to the app.
      if (trace_observer_) {
        const uint32_t tag = proto::getU32LE(pkt.payload, 0);
        const uint32_t auth = proto::getU32LE(pkt.payload, 4);
        trace_observer_->onTrace(tag, auth, pkt.payload[8], pkt.path, snr_count,
                                 &pkt.payload[9], hash_len, pkt.snr);
      }
      return;
    }

    // Otherwise we may be an intermediate hop. Forward only if the next routed
    // hash is ours, this node relays, and we have not already retransmitted it.
    if (!allow_flood_forward_) return;
    const uint8_t entry = uint8_t(1u << path_sz);
    if (std::memcmp(&pkt.payload[9 + offset], self_pub_, entry) != 0) return;
    if (seen_.wasSeen(pkt)) return;
    if (snr_count >= proto::MAX_PATH_SIZE) return;
    seen_.markSeen(pkt);
    pkt.path[snr_count] = uint8_t(pkt.snr);       // append our RX SNR (q4)
    pkt.setPathHashCount(uint8_t(snr_count + 1));  // one more hop recorded
    enqueue(pkt, now + retransmitDelay(5, txAirtime(pkt)), 5);
  }

  void pumpTransmit(uint32_t now) {
    int best = -1;
    for (int i = 0; i < kTxQueueSize; i++) {
      if (!tx_[i].used) continue;
      if (int32_t(now - tx_[i].ready_at) < 0) continue;  // not ready yet
      if (best < 0 || tx_[i].priority < tx_[best].priority ||
          (tx_[i].priority == tx_[best].priority && tx_[i].ready_at < tx_[best].ready_at)) {
        best = i;
      }
    }
    if (best < 0) return;
    const uint32_t airtime = txAirtime(tx_[best].pkt);
    if (!duty_.allows(now)) return;  // hold until duty cycle permits

    uint8_t wire[proto::MAX_TRANS_UNIT];
    size_t len = tx_[best].pkt.writeTo(wire);
    if (radio_->transmit(wire, len)) {
      duty_.record(now, airtime);
      if (tx_[best].priority > 0) forwarded_++;
    }
    tx_[best].used = false;
    tx_count_--;
  }

  void enqueue(const proto::Packet& pkt, uint32_t ready_at, uint8_t priority = 0) {
    for (int i = 0; i < kTxQueueSize; i++) {
      if (!tx_[i].used) {
        tx_[i].pkt = pkt;
        tx_[i].ready_at = ready_at;
        tx_[i].priority = priority;
        tx_[i].used = true;
        tx_count_++;
        return;
      }
    }
    // Queue full: drop (the linker/kernel would surface this as backpressure).
  }

  uint32_t txAirtime(const proto::Packet& pkt) const {
    return timeOnAirMs(pkt.rawLength(), cfg_.bandwidth_khz, cfg_.spreading_factor,
                       cfg_.coding_rate, cfg_.preamble_len);
  }

  static uint32_t retransmitDelay(uint8_t priority, uint32_t airtime) {
    // Lower priority (further away) => longer delay before rebroadcast.
    return airtime * priority;
  }

  RadioDriver* radio_;
  Clock* clock_;
  RadioConfig cfg_;
  uint8_t self_pub_[proto::PUB_KEY_SIZE];

  Dedup seen_;
  FloodRouter router_;
  DutyCycleLimiter duty_{0.5f};  // MeshCore's ~50% airtime-budget default
  bool allow_flood_forward_ = true;
  RawRxObserver* rx_observer_ = nullptr;
  TraceObserver* trace_observer_ = nullptr;

  PacketSink* sinks_[kMaxSinks] = {};
  int sink_count_ = 0;

  TxEntry tx_[kTxQueueSize] = {};
  int tx_count_ = 0;

  uint32_t forwarded_ = 0;
  uint32_t delivered_ = 0;
  uint32_t duplicates_ = 0;
};

}  // namespace corefw
