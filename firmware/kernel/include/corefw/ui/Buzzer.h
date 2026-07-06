// Buzzer / beeps.
//
// Parses RTTTL melodies (the same ringtone format the reference Wio Tracker L1
// companion plays via NonBlockingRTTTL) into note/duration pairs and sequences
// them without blocking, so beeps never stall the radio loop. Tone output is
// abstracted so this is host-testable and board-agnostic.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace corefw::ui {

// A single note: frequency in Hz (0 = rest) and duration in ms.
struct Note {
  uint16_t freq;
  uint16_t ms;
};

inline constexpr int kMaxNotes = 64;

// ToneOutput drives the piezo buzzer. On device this maps to Arduino
// tone()/noTone() on the buzzer pin; in tests a fake captures the calls.
class ToneOutput {
 public:
  virtual ~ToneOutput() = default;
  virtual void tone(uint16_t freq) = 0;
  virtual void noTone() = 0;
};

// noteFrequency returns the frequency of a note given its letter index
// (0=C..11=B) and octave, via equal temperament (A4 = 440 Hz).
inline uint16_t noteFrequency(int semitone, int octave) {
  if (semitone < 0) return 0;  // rest
  int midi = 12 * (octave + 1) + semitone;
  double f = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
  return uint16_t(f + 0.5);
}

// parseRTTTL parses an RTTTL string into `out` (capacity `max`) and returns the
// note count. Format: "name:d=D,o=O,b=B:note,note,...". A note is
// [duration]letter[#][octave][.] where letter is a-g or p (pause).
inline int parseRTTTL(const char* song, Note* out, int max) {
  auto letterSemitone = [](char c) -> int {
    switch (c) {
      case 'c': return 0;
      case 'd': return 2;
      case 'e': return 4;
      case 'f': return 5;
      case 'g': return 7;
      case 'a': return 9;
      case 'b': return 11;
      case 'p': return -1;  // pause
      default: return -2;   // invalid
    }
  };
  const char* p = song;
  // Skip name up to first ':'.
  while (*p && *p != ':') p++;
  if (*p == ':') p++;

  int defDur = 4, defOct = 6, bpm = 63;
  // Defaults section up to next ':'.
  while (*p && *p != ':') {
    if ((p[0] == 'd' || p[0] == 'o' || p[0] == 'b') && p[1] == '=') {
      char key = p[0];
      p += 2;
      int val = 0;
      while (*p >= '0' && *p <= '9') val = val * 10 + (*p++ - '0');
      if (key == 'd') defDur = val;
      else if (key == 'o') defOct = val;
      else bpm = val;
    } else {
      p++;
    }
  }
  if (*p == ':') p++;
  if (bpm <= 0) bpm = 63;

  const double wholeMs = (60000.0 / bpm) * 4.0;
  int count = 0;
  while (*p && count < max && count < kMaxNotes) {
    while (*p == ',' || *p == ' ') p++;
    if (!*p) break;
    // Optional duration.
    int dur = 0;
    while (*p >= '0' && *p <= '9') dur = dur * 10 + (*p++ - '0');
    if (dur == 0) dur = defDur;
    // Note letter.
    int semi = letterSemitone(*p);
    if (semi == -2) {
      p++;
      continue;
    }
    p++;
    if (*p == '#') {
      if (semi >= 0) semi++;
      p++;
    }
    // Optional octave.
    int oct = defOct;
    if (*p >= '0' && *p <= '9') oct = *p++ - '0';
    // Dotted note.
    double mult = 1.0;
    if (*p == '.') {
      mult = 1.5;
      p++;
    }
    uint16_t ms = uint16_t((wholeMs / dur) * mult + 0.5);
    out[count].freq = noteFrequency(semi, oct);
    out[count].ms = ms;
    count++;
  }
  return count;
}

// Melody is a non-blocking player: load a song, then call loop(now_ms) every
// tick. It drives the ToneOutput and advances through notes on schedule.
class Melody {
 public:
  explicit Melody(ToneOutput* out) : out_(out) {}

  // The output can be (re)assigned after construction, since a board attaches
  // its buzzer during startup.
  void setOutput(ToneOutput* out) { out_ = out; }

  void play(const char* song, uint32_t now_ms) {
    count_ = parseRTTTL(song, notes_, kMaxNotes);
    idx_ = 0;
    playing_ = count_ > 0;
    note_ends_ = now_ms;  // start immediately on next loop
  }

  // loop advances the melody; returns true while still playing.
  bool loop(uint32_t now_ms) {
    if (!playing_) return false;
    if (int32_t(now_ms - note_ends_) < 0) return true;  // current note not done
    if (idx_ >= count_ || out_ == nullptr) {
      if (out_) out_->noTone();
      playing_ = false;
      return false;
    }
    const Note& n = notes_[idx_++];
    if (n.freq > 0) out_->tone(n.freq);
    else out_->noTone();
    note_ends_ = now_ms + n.ms;
    return true;
  }

  bool playing() const { return playing_; }
  int noteCount() const { return count_; }
  const Note* notes() const { return notes_; }

 private:
  ToneOutput* out_;
  Note notes_[kMaxNotes] = {};
  int count_ = 0;
  int idx_ = 0;
  bool playing_ = false;
  uint32_t note_ends_ = 0;
};

// A few stock melodies for the companion UI.
namespace melodies {
inline constexpr const char* kStartup = "startup:d=8,o=6,b=120:c,e,g,c7";
inline constexpr const char* kMessage = "msg:d=16,o=6,b=120:g,c7";
inline constexpr const char* kError = "err:d=8,o=5,b=100:c,p,c";
}  // namespace melodies

}  // namespace corefw::ui
