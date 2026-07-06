// Host-side UI tests: RTTTL buzzer sequencing and the companion OLED screen.
//
//   c++ -std=c++17 -I firmware/kernel/include \
//       firmware/kernel/ui/ui_test.cpp -o /tmp/uitest && /tmp/uitest
#include <corefw/ui/Buzzer.h>
#include <corefw/ui/CompanionUI.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace corefw::ui;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

namespace {
class FakeTone : public ToneOutput {
 public:
  void tone(uint16_t f) override {
    freqs.push_back(f);
    current = f;
  }
  void noTone() override { current = 0; }
  std::vector<uint16_t> freqs;
  uint16_t current = 0;
};

class FakeDisplay : public Display {
 public:
  void clear() override {
    lines.clear();
    cleared = true;
  }
  void setCursor(int, int) override {}
  void print(const char* s) override { lines.emplace_back(s); }
  void hline(int) override {}
  void flush() override { flushed = true; }
  bool has(const char* substr) const {
    for (auto& l : lines)
      if (l.find(substr) != std::string::npos) return true;
    return false;
  }
  std::vector<std::string> lines;
  bool cleared = false, flushed = false;
};
}  // namespace

static void testRTTTLParse() {
  Note notes[kMaxNotes];
  int n = parseRTTTL(melodies::kStartup, notes, kMaxNotes);
  check(n == 4, "startup melody has 4 notes");
  // First note is C6 ~= 1047 Hz.
  check(notes[0].freq >= 1040 && notes[0].freq <= 1050, "first note is ~C6");
  for (int i = 0; i < n; i++) check(notes[i].ms > 0, "note has duration");

  // A pause encodes as freq 0.
  Note p[8];
  int m = parseRTTTL("t:d=4,o=5,b=120:c,p,c", p, 8);
  check(m == 3 && p[1].freq == 0, "pause is a rest (freq 0)");
}

static void testMelodySequencing() {
  FakeTone tone;
  Melody mel(&tone);
  mel.play(melodies::kStartup, 0);
  check(mel.playing(), "melody playing after start");

  // First loop emits the first note.
  mel.loop(0);
  check(tone.current > 0, "first note sounding");
  uint16_t first = tone.current;

  // Before the note's duration elapses, nothing changes.
  uint16_t dur0 = mel.notes()[0].ms;
  mel.loop(dur0 / 2);
  check(tone.current == first, "note held for its duration");

  // After it elapses, the next note plays.
  mel.loop(dur0 + 1);
  check(tone.freqs.size() >= 2, "advanced to second note");

  // Drive to completion.
  for (uint32_t t = 0; t < 10000 && mel.playing(); t += 50) mel.loop(t + dur0 + 100);
  check(!mel.playing(), "melody finishes");
  check(tone.current == 0, "buzzer silent at end");
}

static void testCompanionScreen() {
  FakeDisplay d;
  CompanionUI ui;
  ui.setNodeName("Wio Companion");
  ui.setConnected(true);
  ui.setBatteryMilliVolts(3700);
  ui.setUnread(2);
  ui.setLastMessage("hi there");
  ui.render(d);

  check(d.cleared && d.flushed, "screen cleared and flushed");
  check(d.has("Wio Companion"), "shows node name");
  check(d.has("BLE:on"), "shows BLE connected");
  check(d.has("3.70V"), "shows battery voltage");
  check(d.has("Unread: 2"), "shows unread count");
  check(d.has("hi there"), "shows last message");

  // Disconnected state.
  FakeDisplay d2;
  CompanionUI ui2;
  ui2.setConnected(false);
  ui2.setBatteryMilliVolts(4020);
  ui2.render(d2);
  check(d2.has("BLE:off"), "shows BLE disconnected");
  check(d2.has("4.02V"), "battery voltage formatting");
}

int main() {
  testRTTTLParse();
  testMelodySequencing();
  testCompanionScreen();
  std::printf("all UI (buzzer + display) tests passed\n");
  return 0;
}
