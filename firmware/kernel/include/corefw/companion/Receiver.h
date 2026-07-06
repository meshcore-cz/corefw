// MessageReceiver — decrypts inbound Core Protocol packets addressed to this
// node and surfaces them to the companion app.
//
// It mirrors the reference firmware's receive path (Mesh::onRecvPacket ->
// onPeerDataRecv / onGroupDataRecv): a direct datagram is matched to a contact
// by the 1-byte source hash and decrypted with the ECDH shared secret; a group
// datagram is matched to a channel by its hash and decrypted with the channel
// key. The plaintext layout (timestamp(4) || flags || text, txt_type = flags>>2)
// is identical, so messages from existing MeshCore nodes decode correctly.
//
// This is portable and host-testable; results are delivered through a Sink the
// CompanionModule implements.
#pragma once

#include <corefw/companion/State.h>
#include <corefw/protocol/Advert.h>
#include <corefw/protocol/Datagram.h>
#include <corefw/protocol/MessageCrypto.h>
#include <corefw/protocol/Packet.h>

#include <cstring>

namespace corefw::companion {

class MessageReceiver {
 public:
  // Sink receives decoded events. path_len is the flood path length or 0xFF for
  // a direct packet; snr_q4 is the reception SNR × 4 (as the app expects).
  class Sink {
   public:
    virtual ~Sink() = default;
    virtual void onContactMessage(const ContactInfo& from, uint8_t txt_type, uint32_t ts,
                                  uint8_t path_len, int8_t snr_q4, const char* text) = 0;
    virtual void onChannelMessage(uint8_t channel_idx, uint32_t ts, uint8_t path_len,
                                  int8_t snr_q4, const char* channel_name, const char* text) = 0;
    // A verified advert; is_new is true when the contact was just auto-added.
    // encoded_path_len is the wire path_len byte; path_bytes may be null.
    virtual void onAdvert(const ContactInfo& contact, bool is_new, uint8_t encoded_path_len = 0,
                          const uint8_t* path_bytes = nullptr, uint32_t recv_ts = 0) {
      (void)contact; (void)is_new; (void)encoded_path_len; (void)path_bytes; (void)recv_ts;
    }
  };

  MessageReceiver(CompanionState& s, Sink& sink) : s_(s), sink_(sink) {}

  // handle inspects a delivered packet and decrypts/dispatches it if it is for
  // this node. Returns true if it consumed the packet.
  bool handle(const proto::Packet& pkt) {
    switch (pkt.payloadType()) {
      case proto::PAYLOAD_TXT_MSG: return handleDirectText(pkt);
      case proto::PAYLOAD_GRP_TXT: return handleGroupText(pkt);
      case proto::PAYLOAD_ADVERT:  return handleAdvert(pkt);
      default: return false;
    }
  }

 private:
  static uint8_t pathLenField(const proto::Packet& pkt) {
    return pkt.isRouteFlood() ? uint8_t(pkt.pathHashCount()) : 0xFF;
  }

  // Direct text: dest_hash(1) || src_hash(1) || MAC || ciphertext.
  bool handleDirectText(const proto::Packet& pkt) {
    if (pkt.payload_len < 2 + proto::CIPHER_MAC_SIZE) return false;
    if (pkt.payload[0] != s_.self.pub_key[0]) return false;  // not addressed to us
    uint8_t src_hash = pkt.payload[1];

    // Try each contact whose pubkey prefix matches the 1-byte src hash.
    uint8_t plain[proto::MAX_PACKET_PAYLOAD];
    for (int i = 0; i < s_.num_contacts; i++) {
      ContactInfo& c = s_.contacts[i];
      if (c.id.pub_key[0] != src_hash) continue;
      uint8_t secret[proto::PUB_KEY_SIZE];
      s_.self.calcSharedSecret(secret, c.id);
      int n = proto::MACThenDecrypt(secret, plain, &pkt.payload[2], pkt.payload_len - 2);
      if (n <= 5) continue;  // bad MAC for this contact, or too short
      uint32_t ts = proto::getU32LE(plain, 0);
      uint8_t txt_type = uint8_t(plain[4] >> 2);
      plain[n < int(sizeof(plain)) ? n : int(sizeof(plain)) - 1] = 0;  // NUL-terminate text
      sink_.onContactMessage(c, txt_type, ts, pathLenField(pkt), pkt.snr,
                             reinterpret_cast<const char*>(&plain[5]));
      return true;
    }
    return false;
  }

  // Group text: channel_hash(1) || MAC || ciphertext, plaintext "sender: text".
  bool handleGroupText(const proto::Packet& pkt) {
    if (pkt.payload_len < 1 + proto::CIPHER_MAC_SIZE) return false;
    uint8_t ch_hash = pkt.payload[0];
    uint8_t plain[proto::MAX_PACKET_PAYLOAD];
    for (int idx = 0; idx < kMaxGroupChannels; idx++) {
      if (!s_.channel_used[idx]) continue;
      if (s_.channels[idx].channel.hash[0] != ch_hash) continue;
      int n = proto::MACThenDecrypt(s_.channels[idx].channel.secret, plain, &pkt.payload[1],
                                    pkt.payload_len - 1);
      if (n <= 5) continue;
      uint32_t ts = proto::getU32LE(plain, 0);
      plain[n < int(sizeof(plain)) ? n : int(sizeof(plain)) - 1] = 0;
      sink_.onChannelMessage(uint8_t(idx), ts, pathLenField(pkt), pkt.snr,
                             s_.channels[idx].name, reinterpret_cast<const char*>(&plain[5]));
      return true;
    }
    return false;
  }

  // Advert: verify the signature, then auto-add or update the contact.
  bool handleAdvert(const proto::Packet& pkt) {
    proto::Identity id;
    uint32_t timestamp;
    proto::AdvertData ad;
    if (!proto::parseAdvert(pkt, id, timestamp, ad)) return false;
    if (id.pub_key[0] == s_.self.pub_key[0] &&
        std::memcmp(id.pub_key, s_.self.pub_key, proto::PUB_KEY_SIZE) == 0) {
      return true;  // our own advert echoed back
    }

    auto notifyDiscovered = [&](ContactInfo& ci, bool is_new) {
      sink_.onAdvert(ci, is_new, uint8_t(pkt.path_len), pkt.path, timestamp);
    };
    auto populateContact = [&](ContactInfo& c) {
      std::memcpy(c.id.pub_key, id.pub_key, proto::PUB_KEY_SIZE);
      std::strncpy(c.name, ad.name, sizeof(c.name) - 1);
      c.name[sizeof(c.name) - 1] = 0;
      c.type = ad.type;
      c.out_path_len = OUT_PATH_UNKNOWN;
      c.last_advert_timestamp = timestamp;
    };

    ContactInfo* existing = s_.lookupContact(id.pub_key, proto::PUB_KEY_SIZE);
    bool is_new = existing == nullptr;
    if (is_new) {
      if (s_.manual_add_contacts) {
        ContactInfo c;
        populateContact(c);
        notifyDiscovered(c, true);
        return true;
      }
      ContactInfo c;
      populateContact(c);
      if (!s_.addContact(c)) {
        notifyDiscovered(c, true);
        return true;
      }
      existing = &s_.contacts[s_.num_contacts - 1];
    } else {
      existing->last_advert_timestamp = timestamp;
    }
    notifyDiscovered(*existing, is_new);
    return true;
  }

  CompanionState& s_;
  Sink& sink_;
};

}  // namespace corefw::companion
