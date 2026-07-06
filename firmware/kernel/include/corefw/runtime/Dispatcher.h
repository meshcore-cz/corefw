// Dispatcher — the radio scheduler that realises MeshService.
//
// This is the guarded core: modules call send()/subscribe() and never see the
// radio. The dispatcher pumps received frames through the FloodRouter, delivers
// local packets to subscribers, queues forwards, and drains the transmit queue
// onto the radio only when the duty-cycle limiter permits. It owns *when* things
// go on air; modules own *what*.
#pragma once

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
    proto::Packet pkt;
    if (!pkt.readFrom(buf, n)) return;
    pkt.snr = int8_t(radio_->lastSNR() * 4.0f);

    RouteDecision d = router_.route(pkt, self_pub_, seen_);
    if (d.duplicate) {
      duplicates_++;
      return;
    }
    if (d.deliver) {
      delivered_++;
      for (int i = 0; i < sink_count_; i++) sinks_[i]->onPacket(pkt);
    }
    if (d.forward) {
      const uint32_t airtime = txAirtime(pkt);
      // Further-travelled packets wait longer, yielding priority to nodes
      // closer to the source (matches the reference firmware's intent).
      enqueue(pkt, now + retransmitDelay(d.priority, airtime), d.priority);
    }
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
  DutyCycleLimiter duty_{0.01f};

  PacketSink* sinks_[kMaxSinks] = {};
  int sink_count_ = 0;

  TxEntry tx_[kTxQueueSize] = {};
  int tx_count_ = 0;

  uint32_t forwarded_ = 0;
  uint32_t delivered_ = 0;
  uint32_t duplicates_ = 0;
};

}  // namespace corefw
