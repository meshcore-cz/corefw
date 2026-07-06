// Companion Protocol serial frame codec.
//
// Byte-identical to the reference ArduinoSerialInterface: outbound frames are
// '>' + len(LE16) + payload; inbound frames are '<' + len(LE16) + payload. The
// decoder is a small state machine that tolerates split reads (bytes arriving a
// few at a time, as over a UART), matching checkRecvFrame().
#pragma once

#include <corefw/companion/Protocol.h>

#include <cstddef>
#include <cstring>

namespace corefw::companion {

// encodeFrame writes '>' + len(LE16) + payload into out (capacity >= len + 3).
// Returns the total bytes written, or 0 if the payload is too large.
inline size_t encodeFrame(uint8_t* out, const uint8_t* payload, size_t len) {
  if (len > MAX_FRAME_SIZE) return 0;
  out[0] = FRAME_START_OUT;
  out[1] = uint8_t(len & 0xFF);
  out[2] = uint8_t((len >> 8) & 0xFF);
  std::memcpy(&out[3], payload, len);
  return len + 3;
}

// FrameDecoder reassembles inbound ('<' framed) frames from a byte stream that
// may deliver bytes in arbitrary chunks.
class FrameDecoder {
 public:
  // feed one byte. When a complete frame is assembled it is copied into `out`
  // (capacity >= MAX_FRAME_SIZE) and its length returned; otherwise returns 0.
  size_t feed(uint8_t c, uint8_t* out) {
    switch (state_) {
      case Idle:
        if (c == FRAME_START_IN) state_ = HdrFound;
        break;
      case HdrFound:
        len_ = c;  // LSB
        state_ = Len1;
        break;
      case Len1:
        len_ |= uint16_t(c) << 8;  // MSB
        rx_ = 0;
        state_ = (len_ > 0) ? Body : Idle;
        break;
      case Body:
        if (rx_ < MAX_FRAME_SIZE) buf_[rx_] = c;
        rx_++;
        if (rx_ >= len_) {
          uint16_t n = len_;
          if (n > MAX_FRAME_SIZE) n = MAX_FRAME_SIZE;  // truncate oversized
          std::memcpy(out, buf_, n);
          state_ = Idle;
          return n;
        }
        break;
    }
    return 0;
  }

  // Convenience: feed a buffer, invoking `onFrame(payload, len)` for each
  // complete frame. Returns the number of frames emitted.
  template <typename F>
  int feed(const uint8_t* data, size_t len, uint8_t* scratch, F&& onFrame) {
    int frames = 0;
    for (size_t i = 0; i < len; ++i) {
      size_t n = feed(data[i], scratch);
      if (n > 0) {
        onFrame(scratch, n);
        frames++;
      }
    }
    return frames;
  }

  void reset() { state_ = Idle; }

 private:
  enum State { Idle, HdrFound, Len1, Body } state_ = Idle;
  uint16_t len_ = 0;
  uint16_t rx_ = 0;
  uint8_t buf_[MAX_FRAME_SIZE] = {};
};

}  // namespace corefw::companion
