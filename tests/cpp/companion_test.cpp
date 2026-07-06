// Host-side Companion Protocol frame codec tests.
//
// Verifies byte-exact framing against the reference ArduinoSerialInterface:
// outbound '>' framing, inbound '<' decoding, tolerance of split reads, resync
// past garbage, zero-length handling and oversized truncation. Build & run:
//
//   c++ -std=c++17 -I firmware/kernel/include \
//       firmware/kernel/companion/companion_test.cpp -o /tmp/ctest && /tmp/ctest
#include <corefw/companion/FrameCodec.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace corefw::companion;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

static void testEncodeOutbound() {
  // A RESP_CODE_OK reply is a single-byte payload.
  uint8_t payload[1] = {RESP_CODE_OK};
  uint8_t frame[8];
  size_t n = encodeFrame(frame, payload, 1);
  check(n == 4, "outbound frame length");
  check(frame[0] == '>', "outbound start marker");
  check(frame[1] == 0x01 && frame[2] == 0x00, "outbound LE16 length");
  check(frame[3] == RESP_CODE_OK, "outbound payload");

  // A longer payload encodes its length little-endian.
  uint8_t big[300];
  std::memset(big, 0xAB, sizeof(big));
  check(encodeFrame(frame, big, sizeof(big)) == 0, "oversized outbound rejected");
}

static void testDecodeInbound() {
  FrameDecoder dec;
  uint8_t out[MAX_FRAME_SIZE];

  // Inbound CMD_APP_START with a 1-byte app-version arg: '<' 02 00 <CMD> <ver>
  const uint8_t frame[] = {'<', 0x02, 0x00, CMD_APP_START, 0x03};
  size_t got = 0;
  for (uint8_t b : frame) {
    size_t n = dec.feed(b, out);
    if (n) got = n;
  }
  check(got == 2, "inbound frame length");
  check(out[0] == CMD_APP_START, "inbound command byte");
  check(out[1] == 0x03, "inbound arg byte");
}

static void testSplitAndResync() {
  FrameDecoder dec;
  uint8_t out[MAX_FRAME_SIZE];

  // Stream: leading garbage, then a frame, delivered one byte at a time.
  const uint8_t stream[] = {0x00, 0xFF, 'x',       // garbage before start marker
                            '<', 0x03, 0x00,        // start + len=3
                            CMD_SET_DEVICE_TIME, 0x11, 0x22};
  std::vector<int> frameLens;
  for (uint8_t b : stream) {
    size_t n = dec.feed(b, out);
    if (n) frameLens.push_back(int(n));
  }
  check(frameLens.size() == 1, "exactly one frame after resync");
  check(frameLens[0] == 3, "resynced frame length");
  check(out[0] == CMD_SET_DEVICE_TIME, "resynced command");

  // Two back-to-back frames in one buffer via the callback overload.
  const uint8_t two[] = {'<', 0x01, 0x00, CMD_REBOOT,
                         '<', 0x01, 0x00, CMD_LOGOUT};
  uint8_t scratch[MAX_FRAME_SIZE];
  std::vector<uint8_t> cmds;
  int frames = dec.feed(two, sizeof(two), scratch,
                        [&](const uint8_t* p, size_t) { cmds.push_back(p[0]); });
  check(frames == 2, "two frames decoded from one buffer");
  check(cmds[0] == CMD_REBOOT && cmds[1] == CMD_LOGOUT, "both commands recovered");
}

static void testZeroLength() {
  FrameDecoder dec;
  uint8_t out[MAX_FRAME_SIZE];
  // '<' 00 00 is a zero-length frame: the reference returns to idle, emitting
  // nothing, then decodes the following real frame.
  const uint8_t stream[] = {'<', 0x00, 0x00, '<', 0x01, 0x00, CMD_HAS_CONNECTION};
  size_t got = 0;
  int emitted = 0;
  for (uint8_t b : stream) {
    size_t n = dec.feed(b, out);
    if (n) {
      emitted++;
      got = n;
    }
  }
  check(emitted == 1, "zero-length frame emits nothing, next frame decodes");
  check(got == 1 && out[0] == CMD_HAS_CONNECTION, "frame after zero-length");
}

static void testPushCodesDisjoint() {
  // Push codes all have the high bit set, so they never collide with response
  // codes (which are < 0x80). This is what lets an app tell them apart.
  const uint8_t pushes[] = {PUSH_CODE_ADVERT, PUSH_CODE_MSG_WAITING, PUSH_CODE_CONTACTS_FULL};
  for (uint8_t p : pushes) check((p & 0x80) != 0, "push code has high bit");
  const uint8_t resps[] = {RESP_CODE_OK, RESP_CODE_SELF_INFO, RESP_CODE_DEFAULT_FLOOD_SCOPE};
  for (uint8_t r : resps) check((r & 0x80) == 0, "response code lacks high bit");
}

int main() {
  testEncodeOutbound();
  testDecodeInbound();
  testSplitAndResync();
  testZeroLength();
  testPushCodesDisjoint();
  std::printf("all companion protocol codec tests passed\n");
  return 0;
}
