// CompanionModule — the companion role: bridges a phone/app to the mesh.
//
// It ties together the portable, host-tested Companion Protocol pieces: the
// frame codec, the full command handler, the OLED UI and the buzzer. The
// transport (BLE/USB/TCP), display panel, buzzer output, device services and
// radio send path are provided by the board at startup — this class owns the
// orchestration, which is why it stays board-agnostic and testable.
#pragma once

#include <corefw/Mesh.h>
#include <corefw/Module.h>
#include <corefw/companion/Commands.h>
#include <corefw/companion/FrameCodec.h>
#include <corefw/companion/Receiver.h>
#include <corefw/companion/State.h>
#include <corefw/companion/Transport.h>
#include <corefw/protocol/Datagram.h>
#include <corefw/runtime/Clock.h>
#include <corefw/ui/Buzzer.h>
#include <corefw/ui/CompanionUI.h>

#include <cstring>

namespace corefw {

enum class CompanionTransportKind { BLE, USB, WiFi };

inline constexpr int kAdvertPathTableSize = 16;

struct AdvertPath {
  uint8_t pubkey_prefix[7] = {};
  uint8_t path_len = 0;
  char name[32] = {};
  uint32_t recv_timestamp = 0;
  uint8_t path[proto::MAX_PATH_SIZE] = {};
};

// CompanionModule is also a PacketSink (subscribe it to the Dispatcher so mesh
// packets addressed to this node are decrypted) and a MessageReceiver::Sink
// (the decrypted results come back to onContactMessage/onChannelMessage).
class CompanionModule : public Module,
                        public PacketSink,
                        public RawRxObserver,
                        public TraceObserver,
                        public companion::MessageReceiver::Sink {
 public:
  const char* name() const override { return "companion"; }

  // PacketSink: hand each delivered packet to the receiver (which filters by
  // dest hash / channel and decrypts). Returns true if consumed.
  bool onPacket(const proto::Packet& pkt) override { return receiver_.handle(pkt); }

  // RawRxObserver: stream every raw frame off the radio to the app as
  // PUSH_CODE_LOG_RX_DATA (MeshCore MyMesh::logRxRaw). Uses the same
  // non-blocking queue as every other push, so it works on BLE and USB alike.
  void onRawRx(const uint8_t* raw, size_t len, int8_t snr_q4, int8_t rssi) override {
    if (!shouldPushToApp()) return;
    if (len == 0 || len + 3 > companion::MAX_FRAME_SIZE) return;
    uint8_t payload[companion::MAX_FRAME_SIZE];
    payload[0] = companion::PUSH_CODE_LOG_RX_DATA;
    payload[1] = uint8_t(snr_q4);
    payload[2] = uint8_t(rssi);
    std::memcpy(&payload[3], raw, len);
    uint8_t out[companion::MAX_FRAME_SIZE + 3];
    size_t on = companion::encodeFrame(out, payload, len + 3);
    if (on > 0) scheduleIo(out, on);
  }

  // TraceObserver: a TRACE we originated has returned. Emit PUSH_CODE_TRACE_DATA
  // in MeshCore's MyMesh::onTraceRecv layout so the app's Trace Path view can
  // render the per-hop SNRs.
  void onTrace(uint32_t tag, uint32_t auth, uint8_t flags, const uint8_t* snrs,
               uint8_t snr_count, const uint8_t* hashes, uint8_t hash_len,
               int8_t final_snr_q4) override {
    (void)snr_count;
    if (!shouldPushToApp()) return;
    const uint8_t path_sz = flags & 0x03;
    const uint8_t nsnr = uint8_t(hash_len >> path_sz);
    // frame: code, reserved, hash_len, flags, tag(4), auth(4), hashes, snrs, final
    const size_t total = 4u + 4u + 4u + hash_len + nsnr + 1u;
    if (total > companion::MAX_FRAME_SIZE) return;
    uint8_t payload[companion::MAX_FRAME_SIZE];
    size_t i = 0;
    payload[i++] = companion::PUSH_CODE_TRACE_DATA;
    payload[i++] = 0;          // reserved
    payload[i++] = hash_len;   // MeshCore sends the hash-byte length here
    payload[i++] = flags;
    i = proto::putU32LE(payload, i, tag);
    i = proto::putU32LE(payload, i, auth);
    std::memcpy(&payload[i], hashes, hash_len); i += hash_len;
    std::memcpy(&payload[i], snrs, nsnr); i += nsnr;
    payload[i++] = uint8_t(final_snr_q4);
    uint8_t out[companion::MAX_FRAME_SIZE + 3];
    size_t on = companion::encodeFrame(out, payload, i);
    if (on > 0) scheduleIo(out, on);
  }

  // --- Configuration (from generated code) --------------------------------
  void setTransport(const char* t) {
    if (std::strcmp(t, "usb") == 0) transport_kind_ = CompanionTransportKind::USB;
    else if (std::strcmp(t, "wifi") == 0) transport_kind_ = CompanionTransportKind::WiFi;
    else transport_kind_ = CompanionTransportKind::BLE;
  }
  CompanionTransportKind transportKind() const { return transport_kind_; }

  // --- Board-provided services (wired by the target main) -----------------
  void attachTransport(companion::CompanionTransport* io) { io_ = io; }
  void attachHost(companion::CompanionHost* host) { host_ = host; }
  void attachSender(companion::MeshSender* s) { sender_ = s; }
  void attachClock(Clock* clk) { clock_ = clk; }
  void attachDisplay(ui::Display* d) { display_ = d; }
  void attachBuzzer(ui::ToneOutput* b) {
    buzzer_ = b;
    melody_.setOutput(b);
  }

  companion::CompanionState& state() { return state_; }
  ui::CompanionUI& ui() { return ui_; }

  // --- Lifecycle ----------------------------------------------------------
  void initialize(Context& ctx) override {
    ctx_ = &ctx;
    ui_.setNodeName(state_.node_name);
    ui_.setBlePin(state_.ble_pin);
    ui_.setRadio(state_.freq_khz, state_.bw_hz, state_.sf, state_.cr, state_.tx_power_dbm);
    ui_.begin(clock_ ? clock_->millis() : 0);
  }

  // tick pumps the transport, command handling, melody and screen. The target
  // calls this from the main loop; `now_ms` is the monotonic clock.
  void tick(uint32_t now_ms) {
    if (io_) io_->poll();
    flushScheduledIo();
    bool cmd = pumpTransport();
    if (!cmd && state_.contact_sync_active && io_ && host_ && sender_ &&
        io_count_ < kIoQueueDepth - 1) {
      companion::CommandHandler handler(state_, *host_, *sender_);
      TransportWriter writer(this);
      handler.pumpContactSync(writer);
    }
    if (melody_.playing()) melody_.loop(now_ms);
    refreshUI(now_ms);
  }

  // --- Mesh receive path (called by the dispatcher when a message for this
  //     node is decrypted). Builds the offline-queue frame the app fetches with
  //     CMD_SYNC_NEXT_MESSAGE and pushes a MSG_WAITING tickle when connected.

  // A decrypted direct text message from `from`. sender_timestamp/txt_type match
  // the decrypted plaintext; path_len is the flood path length or 0xFF direct.
  void onContactMessage(const companion::ContactInfo& from, uint8_t txt_type,
                        uint32_t sender_timestamp, uint8_t path_len, int8_t snr_q4,
                        const char* text) override {
    uint8_t f[companion::MAX_FRAME_SIZE];
    size_t i = 0;
    if (state_.app_target_ver >= 3) {
      f[i++] = companion::RESP_CODE_CONTACT_MSG_RECV_V3;
      f[i++] = uint8_t(snr_q4);
      f[i++] = 0;  // reserved1
      f[i++] = 0;  // reserved2
    } else {
      f[i++] = companion::RESP_CODE_CONTACT_MSG_RECV;
    }
    std::memcpy(&f[i], from.id.pub_key, 6); i += 6;
    f[i++] = path_len;
    f[i++] = txt_type;
    i = proto::putU32LE(f, i, sender_timestamp);
    size_t tlen = std::strlen(text);
    if (i + tlen > companion::MAX_FRAME_SIZE) tlen = companion::MAX_FRAME_SIZE - i;
    std::memcpy(&f[i], text, tlen); i += tlen;
    enqueueAndNotify(f, int(i), from.name, text, path_len,
                     txt_type == proto::TXT_TYPE_PLAIN || txt_type == proto::TXT_TYPE_SIGNED_PLAIN);
  }

  // A decrypted channel text message on `channel_idx`.
  void onChannelMessage(uint8_t channel_idx, uint32_t timestamp, uint8_t path_len,
                        int8_t snr_q4, const char* channel_name, const char* text) override {
    uint8_t f[companion::MAX_FRAME_SIZE];
    size_t i = 0;
    if (state_.app_target_ver >= 3) {
      f[i++] = companion::RESP_CODE_CHANNEL_MSG_RECV_V3;
      f[i++] = uint8_t(snr_q4);
      f[i++] = 0;
      f[i++] = 0;
    } else {
      f[i++] = companion::RESP_CODE_CHANNEL_MSG_RECV;
    }
    f[i++] = channel_idx;
    f[i++] = path_len;
    f[i++] = proto::TXT_TYPE_PLAIN;
    i = proto::putU32LE(f, i, timestamp);
    size_t tlen = std::strlen(text);
    if (i + tlen > companion::MAX_FRAME_SIZE) tlen = companion::MAX_FRAME_SIZE - i;
    std::memcpy(&f[i], text, tlen); i += tlen;
    enqueueAndNotify(f, int(i), channel_name, text, path_len, true);
  }

  // A verified advert. Push NEW_ADVERT (full contact) for a freshly-added
  // contact, or ADVERT (pubkey only) for a refresh, matching the reference.
  void onAdvert(const companion::ContactInfo& contact, bool is_new, uint8_t encoded_path_len,
                const uint8_t* path_bytes, uint32_t recv_ts) override {
    if (is_new) dirty_ = true;
    ui_.addRecentAdvert(contact.name, host_ ? host_->rtcNow() : 0);
    recordAdvertPath(contact, encoded_path_len, path_bytes, recv_ts);
    if (!shouldPushToApp()) return;
    uint8_t f[companion::MAX_FRAME_SIZE];
    size_t n;
    if (is_new) {
      n = companion::writeContactRespFrame(companion::PUSH_CODE_NEW_ADVERT, contact, f);
    } else {
      f[0] = companion::PUSH_CODE_ADVERT;
      std::memcpy(&f[1], contact.id.pub_key, proto::PUB_KEY_SIZE);
      n = 1 + proto::PUB_KEY_SIZE;
    }
    uint8_t out[companion::MAX_FRAME_SIZE + 3];
    size_t on = companion::encodeFrame(out, f, n);
    if (on > 0) scheduleIo(out, on);
  }

  // A PAYLOAD_ACK (direct or embedded in a path return) for a message we sent:
  // confirm delivery to the app with the round-trip time (PUSH_CODE_SEND_CONFIRMED).
  void onAck(uint32_t crc) override {
    uint32_t sent_s = 0;
    if (!state_.matchExpectedAck(crc, sent_s)) return;  // not one we're waiting on
    if (!shouldPushToApp()) return;
    uint8_t f[9];
    f[0] = companion::PUSH_CODE_SEND_CONFIRMED;
    proto::putU32LE(f, 1, crc);
    uint32_t trip_ms = host_ ? (host_->rtcNow() - sent_s) * 1000u : 0;
    proto::putU32LE(f, 5, trip_ms);
    pushFrame(f, sizeof(f));
  }

  // A contact's return path was learned/updated: persist it and tell the app so
  // future sends to it go direct (PUSH_CODE_PATH_UPDATED).
  void onPathUpdated(const companion::ContactInfo& contact) override {
    dirty_ = true;
    if (!shouldPushToApp()) return;
    uint8_t f[1 + proto::PUB_KEY_SIZE];
    f[0] = companion::PUSH_CODE_PATH_UPDATED;
    std::memcpy(&f[1], contact.id.pub_key, proto::PUB_KEY_SIZE);
    pushFrame(f, sizeof(f));
  }

  // A raw control payload (MeshCore onControlDataRecv → PUSH_CODE_CONTROL_DATA).
  void onControl(const uint8_t* data, size_t len, uint8_t path_len, int8_t snr_q4) override {
    if (!shouldPushToApp()) return;
    if (len + 4 > companion::MAX_FRAME_SIZE) return;
    uint8_t f[companion::MAX_FRAME_SIZE];
    size_t i = 0;
    f[i++] = companion::PUSH_CODE_CONTROL_DATA;
    f[i++] = uint8_t(snr_q4);
    f[i++] = 0;  // rssi (not tracked per-packet here)
    f[i++] = path_len;
    std::memcpy(&f[i], data, len);
    i += len;
    pushFrame(f, i);
  }

  // We decrypted a plain text and must acknowledge the sender; the board sender
  // routes the ACK direct (via the contact's out_path) or by flood.
  void needAck(const companion::ContactInfo& to, const uint8_t* ack, uint8_t ack_len,
               bool via_flood) override {
    (void)via_flood;  // the board sender picks direct vs flood from to.out_path_len
    if (sender_ != nullptr) sender_->sendAck(ack, ack_len, to);
  }

  // A decrypted PAYLOAD_RESPONSE: match it to a pending login / status /
  // telemetry / binary request and push the result (MeshCore onContactResponse).
  void onResponse(const companion::ContactInfo& from, const uint8_t* data, size_t len) override {
    if (len < 4 || !shouldPushToApp()) return;
    const uint32_t tag = proto::getU32LE(data, 0);
    const uint32_t key4 = proto::getU32LE(from.id.pub_key, 0);

    // Login reply (matched on the repeater's pub-key prefix, legacy scheme).
    if (state_.pending_login != 0 && state_.pending_login == key4) {
      state_.pending_login = 0;
      uint8_t f[companion::MAX_FRAME_SIZE];
      size_t i = 0;
      bool ok_legacy = (len >= 6 && data[4] == 'O' && data[5] == 'K');
      bool ok_new = (len >= 5 && data[4] == 0);  // RESP_SERVER_LOGIN_OK
      if (ok_legacy) {
        f[i++] = companion::PUSH_CODE_LOGIN_SUCCESS;
        f[i++] = 0;  // is_admin (legacy)
        std::memcpy(&f[i], from.id.pub_key, 6); i += 6;
      } else if (ok_new) {
        f[i++] = companion::PUSH_CODE_LOGIN_SUCCESS;
        f[i++] = len > 6 ? data[6] : 0;  // permissions (is_admin)
        std::memcpy(&f[i], from.id.pub_key, 6); i += 6;
        proto::putU32LE(f, i, tag); i += 4;   // server timestamp
        f[i++] = len > 7 ? data[7] : 0;   // ACL permissions
        f[i++] = len > 12 ? data[12] : 0;  // firmware version level
      } else {
        f[i++] = companion::PUSH_CODE_LOGIN_FAIL;
        f[i++] = 0;  // reserved
        std::memcpy(&f[i], from.id.pub_key, 6); i += 6;
      }
      pushFrame(f, i);
      return;
    }
    if (len <= 4) return;
    // Status reply (matched on pub-key prefix).
    if (state_.pending_status != 0 && state_.pending_status == key4) {
      state_.pending_status = 0;
      emitContactResponse(companion::PUSH_CODE_STATUS_RESPONSE, from, &data[4], len - 4);
      return;
    }
    // Telemetry reply (matched on request tag).
    if (state_.pending_telemetry != 0 && tag == state_.pending_telemetry) {
      state_.pending_telemetry = 0;
      emitContactResponse(companion::PUSH_CODE_TELEMETRY_RESPONSE, from, &data[4], len - 4);
      return;
    }
    // Binary reply (matched on request tag): [code, tag(4), reply].
    if (state_.pending_req != 0 && tag == state_.pending_req) {
      state_.pending_req = 0;
      uint8_t f[companion::MAX_FRAME_SIZE];
      size_t i = 0;
      f[i++] = companion::PUSH_CODE_BINARY_RESPONSE;
      proto::putU32LE(f, i, tag); i += 4;
      size_t dl = len - 4;
      if (i + dl > companion::MAX_FRAME_SIZE) dl = companion::MAX_FRAME_SIZE - i;
      std::memcpy(&f[i], &data[4], dl); i += dl;
      pushFrame(f, i);
      return;
    }
  }

  void onEvent(const Event& e) override {
    if (e.type == EventType::CompanionConnected) {
      connected_ = true;
      ui_.setConnected(true);
      ui_.setSerialEnabled(true);
    } else if (e.type == EventType::CompanionDisconnected) {
      connected_ = false;
      ui_.setConnected(false);
    }
  }

  bool connected() const { return connected_; }

  // GET_ADVERT_PATH lookup (MeshCore advert_paths[] table).
  int advertPath(const uint8_t* pub_key, uint32_t& recv_ts, uint8_t* path) const {
    for (int i = 0; i < kAdvertPathTableSize; i++) {
      if (std::memcmp(advert_paths_[i].pubkey_prefix, pub_key, sizeof(advert_paths_[i].pubkey_prefix)) == 0) {
        recv_ts = advert_paths_[i].recv_timestamp;
        uint8_t bl = uint8_t((advert_paths_[i].path_len & 63) * (((advert_paths_[i].path_len >> 6) + 1)));
        std::memcpy(path, advert_paths_[i].path, bl);
        return bl;
      }
    }
    return -1;
  }

  // Play the boot chime once hardware is ready (MeshCore UITask::begin()).
  void playStartupMelody() {
    if (buzzer_ != nullptr && clock_ != nullptr) {
      melody_.play(ui::melodies::kStartup, clock_->millis());
    }
  }

 private:
  static constexpr int kIoQueueDepth = 8;

  // TransportWriter frames each handler response and queues it for the transport.
  class TransportWriter : public companion::FrameWriter {
   public:
    explicit TransportWriter(CompanionModule* mod) : mod_(mod) {}
    void writeFrame(const uint8_t* data, size_t len) override {
      if (!mod_ || !mod_->io_) return;
      uint8_t out[companion::MAX_FRAME_SIZE + 3];
      size_t n = companion::encodeFrame(out, data, len);
      if (n > 0) mod_->scheduleIo(out, n);
    }
   private:
    CompanionModule* mod_;
  };

  // Read inbound bytes, decode frames, run the command handler and reply.
  // Processes at most one command per call (MeshCore checkSerialInterface).
  bool pumpTransport() {
    if (io_ == nullptr || host_ == nullptr || sender_ == nullptr) return false;
    uint8_t in[companion::MAX_FRAME_SIZE];
    size_t n = io_->read(in, sizeof(in));
    if (n == 0) return false;
    companion::CommandHandler handler(state_, *host_, *sender_);
    TransportWriter writer(this);
    bool handled = false;
    decoder_.feed(in, n, frame_, [&](const uint8_t* payload, size_t plen) {
      if (handled) return;
      handled = true;
      handler.handle(payload, plen, writer);
    });
    return handled;
  }

  bool shouldPushToApp() const {
    if (io_ == nullptr) return false;
    if (transport_kind_ == CompanionTransportKind::USB) return true;
    return connected_;
  }

  void enqueueAndNotify(const uint8_t* frame, int len, const char* who, const char* text,
                        uint8_t path_len, bool display) {
    state_.pushOffline(frame, len);
    unread_++;
    ui_.setUnread(unread_);
    if (display) {
      ui_.addMessagePreview(path_len, who, text, host_ ? host_->rtcNow() : 0);
      beep(ui::melodies::kMessage);
    }
    (void)who; (void)path_len;
    if (shouldPushToApp()) {
      uint8_t tickle[1] = {companion::PUSH_CODE_MSG_WAITING};
      uint8_t out[4];
      size_t nn = companion::encodeFrame(out, tickle, 1);
      if (nn > 0) scheduleIo(out, nn);
    }
  }

  void recordAdvertPath(const companion::ContactInfo& contact, uint8_t encoded_path_len,
                        const uint8_t* path_bytes, uint32_t recv_ts) {
    if (path_bytes == nullptr || !proto::Packet::isValidPathLen(encoded_path_len)) return;
    AdvertPath* slot = advert_paths_;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < kAdvertPathTableSize; i++) {
      if (std::memcmp(advert_paths_[i].pubkey_prefix, contact.id.pub_key,
                      sizeof(advert_paths_[i].pubkey_prefix)) == 0) {
        slot = &advert_paths_[i];
        break;
      }
      if (advert_paths_[i].recv_timestamp < oldest) {
        oldest = advert_paths_[i].recv_timestamp;
        slot = &advert_paths_[i];
      }
    }
    std::memcpy(slot->pubkey_prefix, contact.id.pub_key, sizeof(slot->pubkey_prefix));
    std::strncpy(slot->name, contact.name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = 0;
    slot->recv_timestamp = recv_ts;
    slot->path_len = encoded_path_len;
    uint8_t bl = uint8_t((encoded_path_len & 63) * (((encoded_path_len >> 6) + 1)));
    std::memcpy(slot->path, path_bytes, bl);
  }

  // Push a [code, 0(reserved), pub_key_prefix(6), reply...] response frame
  // (MeshCore status/telemetry response layout).
  void emitContactResponse(uint8_t code, const companion::ContactInfo& from,
                           const uint8_t* reply, size_t reply_len) {
    uint8_t f[companion::MAX_FRAME_SIZE];
    size_t i = 0;
    f[i++] = code;
    f[i++] = 0;  // reserved
    std::memcpy(&f[i], from.id.pub_key, 6); i += 6;
    if (i + reply_len > companion::MAX_FRAME_SIZE) reply_len = companion::MAX_FRAME_SIZE - i;
    std::memcpy(&f[i], reply, reply_len); i += reply_len;
    pushFrame(f, i);
  }

  // Frame a push payload and queue it on the non-blocking IO ring.
  void pushFrame(const uint8_t* payload, size_t len) {
    uint8_t out[companion::MAX_FRAME_SIZE + 3];
    size_t on = companion::encodeFrame(out, payload, len);
    if (on > 0) scheduleIo(out, on);
  }

  void scheduleIo(const uint8_t* framed, size_t len) {
    if (len > sizeof(io_queue_[0]) || io_count_ >= kIoQueueDepth) return;
    std::memcpy(io_queue_[io_tail_], framed, len);
    io_queue_len_[io_tail_] = len;
    io_tail_ = (io_tail_ + 1) % kIoQueueDepth;
    io_count_++;
  }

  void flushScheduledIo() {
    if (io_ == nullptr || io_count_ == 0) return;
    while (io_count_ > 0) {
      size_t len = io_queue_len_[io_head_];
      size_t sent = io_->writePartial(io_queue_[io_head_] + io_partial_off_,
                                       len - io_partial_off_);
      if (sent == 0) return;
      io_partial_off_ += sent;
      if (io_partial_off_ < len) return;
      io_partial_off_ = 0;
      io_head_ = (io_head_ + 1) % kIoQueueDepth;
      io_count_--;
    }
  }

  void refreshUI(uint32_t now_ms) {
    if (display_ == nullptr) return;
    if (now_ms < next_render_ && !dirty_ && !ui_.dirty()) return;
    if (host_ != nullptr) ui_.setBatteryMilliVolts(host_->batteryMilliVolts());
    ui_.setNodeName(state_.node_name);
    ui_.setBlePin(state_.ble_pin);
    ui_.setRadio(state_.freq_khz, state_.bw_hz, state_.sf, state_.cr, state_.tx_power_dbm);
    if (host_ != nullptr) {
      ui_.setNoiseFloorDbm(host_->radioNoiseFloorDbm());
      ui_.setGps(host_->gpsEnabled(), host_->gpsHasFix(), host_->gpsSatellites(),
                 state_.lat_e6, state_.lon_e6);
    }
    uint32_t delay = ui_.render(*display_, now_ms, host_ ? host_->rtcNow() : 0);
    next_render_ = now_ms + delay;
    dirty_ = false;
  }

  void beep(const char* song) {
    if (buzzer_ == nullptr || clock_ == nullptr) return;
    melody_.play(song, clock_->millis());
  }

  Context* ctx_ = nullptr;
  CompanionTransportKind transport_kind_ = CompanionTransportKind::BLE;

  companion::CompanionTransport* io_ = nullptr;
  companion::CompanionHost* host_ = nullptr;
  companion::MeshSender* sender_ = nullptr;
  Clock* clock_ = nullptr;
  ui::Display* display_ = nullptr;
  ui::ToneOutput* buzzer_ = nullptr;

  companion::CompanionState state_;
  companion::MessageReceiver receiver_{state_, *this};
  companion::FrameDecoder decoder_;
  uint8_t frame_[companion::MAX_FRAME_SIZE] = {};

  ui::CompanionUI ui_;
  ui::Melody melody_{nullptr};

  bool connected_ = false;
  int unread_ = 0;
  bool dirty_ = true;
  uint32_t next_render_ = 0;
  AdvertPath advert_paths_[kAdvertPathTableSize] = {};
  uint8_t io_queue_[kIoQueueDepth][companion::MAX_FRAME_SIZE + 3] = {};
  size_t io_queue_len_[kIoQueueDepth] = {};
  int io_head_ = 0;
  int io_tail_ = 0;
  int io_count_ = 0;
  size_t io_partial_off_ = 0;
};

}  // namespace corefw
