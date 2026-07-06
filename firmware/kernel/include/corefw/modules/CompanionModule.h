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

// CompanionModule is also a PacketSink (subscribe it to the Dispatcher so mesh
// packets addressed to this node are decrypted) and a MessageReceiver::Sink
// (the decrypted results come back to onContactMessage/onChannelMessage).
class CompanionModule : public Module,
                        public PacketSink,
                        public companion::MessageReceiver::Sink {
 public:
  const char* name() const override { return "companion"; }

  // PacketSink: hand each delivered packet to the receiver (which filters by
  // dest hash / channel and decrypts). Returns true if consumed.
  bool onPacket(const proto::Packet& pkt) override { return receiver_.handle(pkt); }

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
  }

  // tick pumps the transport, command handling, melody and screen. The target
  // calls this from the main loop; `now_ms` is the monotonic clock.
  void tick(uint32_t now_ms) {
    pumpTransport();
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
  void onAdvert(const companion::ContactInfo& contact, bool is_new) override {
    if (is_new) dirty_ = true;
    if (!connected_ || io_ == nullptr) return;
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
    if (on > 0) io_->write(out, on);
  }

  void onEvent(const Event& e) override {
    if (e.type == EventType::CompanionConnected) {
      connected_ = true;
      ui_.setConnected(true);
      beep(ui::melodies::kStartup);
    } else if (e.type == EventType::CompanionDisconnected) {
      connected_ = false;
      ui_.setConnected(false);
    }
  }

  bool connected() const { return connected_; }

 private:
  // TransportWriter frames each handler response and writes it to the transport.
  class TransportWriter : public companion::FrameWriter {
   public:
    explicit TransportWriter(companion::CompanionTransport* io) : io_(io) {}
    void writeFrame(const uint8_t* data, size_t len) override {
      if (!io_) return;
      uint8_t out[companion::MAX_FRAME_SIZE + 3];
      size_t n = companion::encodeFrame(out, data, len);
      if (n > 0) io_->write(out, n);
    }
   private:
    companion::CompanionTransport* io_;
  };

  // Read inbound bytes, decode frames, run the command handler and reply.
  void pumpTransport() {
    if (io_ == nullptr || host_ == nullptr || sender_ == nullptr) return;
    uint8_t in[companion::MAX_FRAME_SIZE];
    size_t n = io_->read(in, sizeof(in));
    if (n == 0) return;
    companion::CommandHandler handler(state_, *host_, *sender_);
    TransportWriter writer(io_);
    decoder_.feed(in, n, frame_, [&](const uint8_t* payload, size_t plen) {
      handler.handle(payload, plen, writer);
    });
  }

  void enqueueAndNotify(const uint8_t* frame, int len, const char* who, const char* text,
                        uint8_t path_len, bool display) {
    state_.pushOffline(frame, len);
    unread_++;
    ui_.setUnread(unread_);
    if (display) {
      if (text) ui_.setLastMessage(text);
      beep(ui::melodies::kMessage);
    }
    (void)who; (void)path_len;
    if (connected_ && io_) {
      uint8_t tickle[1] = {companion::PUSH_CODE_MSG_WAITING};
      uint8_t out[4];
      size_t nn = companion::encodeFrame(out, tickle, 1);
      io_->write(out, nn);
    }
  }

  void refreshUI(uint32_t now_ms) {
    if (display_ == nullptr) return;
    if (now_ms - last_render_ < 250 && !dirty_) return;  // ~4 Hz refresh
    if (host_ != nullptr) ui_.setBatteryMilliVolts(host_->batteryMilliVolts());
    ui_.render(*display_);
    last_render_ = now_ms;
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
  uint32_t last_render_ = 0;
};

}  // namespace corefw
