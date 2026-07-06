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
    // A PAYLOAD_ACK (or an ACK embedded in a path return) matching a message we
    // sent: `crc` is the 4-byte ack hash. The companion confirms delivery.
    virtual void onAck(uint32_t crc) { (void)crc; }
    // A PAYLOAD_PATH updated `contact`'s return path (out_path now known).
    virtual void onPathUpdated(const ContactInfo& contact) { (void)contact; }
    // A PAYLOAD_CONTROL broadcast payload (raw control bytes).
    virtual void onControl(const uint8_t* data, size_t len, uint8_t path_len, int8_t snr_q4) {
      (void)data; (void)len; (void)path_len; (void)snr_q4;
    }
    // We received & decrypted a PLAIN direct text and must acknowledge the
    // sender so it learns the message was delivered (MeshCore onPeerDataRecv).
    virtual void needAck(const ContactInfo& to, const uint8_t* ack, uint8_t ack_len,
                         bool via_flood) {
      (void)to; (void)ack; (void)ack_len; (void)via_flood;
    }
    // A decrypted PAYLOAD_RESPONSE from a contact: reply to a login / status /
    // telemetry request. data is tag(4) ‖ reply. Mirrors onContactResponse.
    virtual void onResponse(const ContactInfo& from, const uint8_t* data, size_t len) {
      (void)from; (void)data; (void)len;
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
      case proto::PAYLOAD_ACK:     return handleAck(pkt);
      case proto::PAYLOAD_PATH:    return handlePath(pkt);
      case proto::PAYLOAD_CONTROL: return handleControl(pkt);
      case proto::PAYLOAD_RESPONSE: return handleResponse(pkt);
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
      // Acknowledge a plain text so the sender gets a delivery receipt. The ack
      // CRC is keyed with the sender's pub_key over its plaintext, so it matches
      // the sender's expected_ack (MeshCore onPeerDataRecv).
      if (txt_type == proto::TXT_TYPE_PLAIN) {
        size_t tlen = std::strlen(reinterpret_cast<const char*>(&plain[5]));
        uint32_t crc = proto::ackHashFor(plain, 5 + tlen, c.id.pub_key);
        uint8_t ack[4];
        proto::putU32LE(ack, 0, crc);
        sink_.needAck(c, ack, sizeof(ack), pkt.isRouteFlood());
      }
      return true;
    }
    return false;
  }

  // PAYLOAD_ACK: a 4-byte ack hash confirming a message we sent was delivered.
  bool handleAck(const proto::Packet& pkt) {
    if (pkt.payload_len < 4) return false;
    sink_.onAck(proto::getU32LE(pkt.payload, 0));
    return true;
  }

  // PAYLOAD_RESPONSE: an encrypted reply from a contact to a login / status /
  // telemetry / binary request. Decrypt with the matching contact and surface it.
  bool handleResponse(const proto::Packet& pkt) {
    if (pkt.payload_len < 2 + proto::CIPHER_MAC_SIZE) return false;
    if (pkt.payload[0] != s_.self.pub_key[0]) return false;  // not addressed to us
    uint8_t src_hash = pkt.payload[1];
    uint8_t plain[proto::MAX_PACKET_PAYLOAD];
    for (int i = 0; i < s_.num_contacts; i++) {
      ContactInfo& c = s_.contacts[i];
      if (c.id.pub_key[0] != src_hash) continue;
      uint8_t secret[proto::PUB_KEY_SIZE];
      s_.self.calcSharedSecret(secret, c.id);
      int n = proto::MACThenDecrypt(secret, plain, &pkt.payload[2], pkt.payload_len - 2);
      if (n <= 0) continue;  // bad MAC for this contact
      sink_.onResponse(c, plain, size_t(n));
      return true;
    }
    return false;
  }

  // PAYLOAD_CONTROL: a zero-hop broadcast control payload (high bit of byte 0
  // set, per MeshCore onControlDataRecv). Surface it raw to the app.
  bool handleControl(const proto::Packet& pkt) {
    if (pkt.payload_len < 1 || (pkt.payload[0] & 0x80) == 0) return false;
    sink_.onControl(pkt.payload, pkt.payload_len, uint8_t(pkt.pathHashCount()), pkt.snr);
    return true;
  }

  // PAYLOAD_PATH: an encrypted return path from a contact. Decrypt, store it as
  // the contact's out_path (so future sends go direct), and surface any ACK
  // embedded in the path's `extra` field (MeshCore onContactPathRecv).
  bool handlePath(const proto::Packet& pkt) {
    if (pkt.payload_len < 2 + proto::CIPHER_MAC_SIZE) return false;
    if (pkt.payload[0] != s_.self.pub_key[0]) return false;  // not addressed to us
    uint8_t src_hash = pkt.payload[1];
    uint8_t plain[proto::MAX_PACKET_PAYLOAD];
    for (int i = 0; i < s_.num_contacts; i++) {
      ContactInfo& c = s_.contacts[i];
      if (c.id.pub_key[0] != src_hash) continue;
      uint8_t secret[proto::PUB_KEY_SIZE];
      s_.self.calcSharedSecret(secret, c.id);
      int n = proto::MACThenDecrypt(secret, plain, &pkt.payload[2], pkt.payload_len - 2);
      if (n <= 0) continue;  // bad MAC for this contact
      // plain: path_len(1) || path[hash_size*hash_count] || extra_type(1) || extra
      int k = 0;
      uint8_t path_len = plain[k++];
      if (!proto::Packet::isValidPathLen(path_len)) return true;
      uint8_t hash_size = uint8_t((path_len >> 6) + 1);
      uint8_t hash_count = uint8_t(path_len & 63);
      int bl = int(hash_size) * hash_count;
      if (k + bl + 1 > n) return true;  // malformed
      uint8_t copy = uint8_t(bl <= int(sizeof(c.out_path)) ? bl : int(sizeof(c.out_path)));
      std::memcpy(c.out_path, &plain[k], copy);
      c.out_path_len = path_len;  // stored as the app expects (raw path_len byte)
      k += bl;
      uint8_t extra_type = uint8_t(plain[k++] & 0x0F);
      const uint8_t* extra = &plain[k];
      int extra_len = n - k;
      sink_.onPathUpdated(c);
      if (extra_type == proto::PAYLOAD_ACK && extra_len >= 4) {
        sink_.onAck(proto::getU32LE(extra, 0));  // path return carried an ACK
      }
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
      // allow_evict=false: an advert must never evict an existing contact. On a
      // full table we just surface the discovery without storing it.
      if (!s_.addContact(c, nullptr, nullptr, /*allow_evict=*/false)) {
        notifyDiscovered(c, true);
        return true;
      }
      // addContact may append or (when full) overwrite an existing slot, so find
      // the contact by key rather than assuming it landed at the end.
      existing = s_.lookupContact(id.pub_key, proto::PUB_KEY_SIZE);
      if (!existing) return true;
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
