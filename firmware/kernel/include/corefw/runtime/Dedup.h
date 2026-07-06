// Duplicate suppression.
//
// A fixed-size ring of recently-seen packet hashes. Because a mesh floods
// packets, the same packet arrives many times; the router drops any it has
// already seen. State is node-local, so this never needs to agree across nodes
// — only the hash function (Core Protocol packet hash) is compatibility-
// relevant, and that is shared via PacketHash.h.
#pragma once

#include <corefw/protocol/PacketHash.h>

namespace corefw {

// Matches the reference firmware's SimpleMeshTables capacity (128 + 32).
inline constexpr int kMaxPacketHashes = 160;

class Dedup {
 public:
  bool wasSeen(const proto::Packet& pkt) const {
    uint8_t hash[proto::MAX_HASH_SIZE];
    proto::calculatePacketHash(pkt, hash);
    return indexOf(hash) >= 0;
  }

  void markSeen(const proto::Packet& pkt) {
    uint8_t hash[proto::MAX_HASH_SIZE];
    proto::calculatePacketHash(pkt, hash);
    if (indexOf(hash) >= 0) return;  // already present
    std::memcpy(&hashes_[next_ * proto::MAX_HASH_SIZE], hash, proto::MAX_HASH_SIZE);
    next_ = (next_ + 1) % kMaxPacketHashes;
  }

  // Test-and-set: returns true if the packet was already seen; otherwise records
  // it and returns false. Convenient for the router's fast path.
  bool checkAndMark(const proto::Packet& pkt) {
    uint8_t hash[proto::MAX_HASH_SIZE];
    proto::calculatePacketHash(pkt, hash);
    if (indexOf(hash) >= 0) return true;
    std::memcpy(&hashes_[next_ * proto::MAX_HASH_SIZE], hash, proto::MAX_HASH_SIZE);
    next_ = (next_ + 1) % kMaxPacketHashes;
    return false;
  }

 private:
  int indexOf(const uint8_t* hash) const {
    const uint8_t* sp = hashes_;
    for (int i = 0; i < kMaxPacketHashes; i++, sp += proto::MAX_HASH_SIZE) {
      if (std::memcmp(hash, sp, proto::MAX_HASH_SIZE) == 0) return i;
    }
    return -1;
  }

  uint8_t hashes_[kMaxPacketHashes * proto::MAX_HASH_SIZE] = {};
  int next_ = 0;
};

}  // namespace corefw
