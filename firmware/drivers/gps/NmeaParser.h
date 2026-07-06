// NmeaParser — a tiny, portable NMEA-0183 decoder for GNSS receivers such as
// the Wio Tracker L1's L76KB. It is feed-a-byte-at-a-time and dependency-free,
// so it is host-testable (no Arduino/Stream). It understands the two sentences
// the companion cares about: RMC (position, validity, date+time) and GGA (fix
// quality). Positions are exposed as integer micro-degrees to match the
// companion state and MeshCore's node_lat/node_lon storage.
//
// This is deliberately not a full NMEA implementation; it decodes exactly the
// fields the mesh uses and ignores everything else, matching the behaviour of
// MeshCore's MicroNMEALocationProvider for our purposes.
#pragma once

#include <cstdint>
#include <cstring>

namespace corefw::gps {

class NmeaParser {
 public:
  // feed one received character. Returns true when a complete, valid sentence
  // has just been parsed and updated the fix state.
  bool feed(char c) {
    if (c == '$') {  // start of a sentence
      len_ = 0;
      collecting_ = true;
      return false;
    }
    if (!collecting_) return false;
    if (c == '\r' || c == '\n') {
      collecting_ = false;
      if (len_ == 0) return false;
      line_[len_] = 0;
      return parseLine();
    }
    if (len_ < kMaxLine - 1) {
      line_[len_++] = c;
    } else {
      collecting_ = false;  // overrun; drop the sentence
    }
    return false;
  }

  bool hasFix() const { return has_fix_; }
  int32_t latE6() const { return lat_e6_; }
  int32_t lonE6() const { return lon_e6_; }
  // Unix time from the last RMC with a valid date, or 0 if never seen.
  uint32_t unixTime() const { return unix_time_; }
  uint8_t satellites() const { return sats_; }

 private:
  static constexpr int kMaxLine = 96;
  static constexpr int kMaxFields = 20;

  // Split line_ on commas (up to the '*' checksum) into field pointers.
  int split(const char** fields) {
    int n = 0;
    fields[n++] = line_;
    for (int i = 0; line_[i] && n < kMaxFields; i++) {
      if (line_[i] == '*') { line_[i] = 0; break; }
      if (line_[i] == ',') {
        line_[i] = 0;
        fields[n++] = &line_[i + 1];
      }
    }
    return n;
  }

  // Talker-agnostic sentence match: skip the 2-char talker id ("GP","GN",...).
  static bool isType(const char* tag, const char* type3) {
    return tag[0] && tag[1] && std::strncmp(tag + 2, type3, 3) == 0;
  }

  bool parseLine() {
    const char* f[kMaxFields] = {};
    int n = split(f);
    if (n < 1) return false;
    if (isType(f[0], "RMC")) return parseRmc(f, n);
    if (isType(f[0], "GGA")) return parseGga(f, n);
    return false;
  }

  // $--RMC,time,status,lat,N/S,lon,E/W,speed,course,date,...
  bool parseRmc(const char** f, int n) {
    if (n < 10) return false;
    bool valid = f[2][0] == 'A';
    if (!valid) { has_fix_ = false; return false; }
    int32_t lat = degMin(f[3], f[4][0]);
    int32_t lon = degMin(f[5], f[6][0]);
    if (lat == kInvalid || lon == kInvalid) return false;
    lat_e6_ = lat;
    lon_e6_ = lon;
    has_fix_ = true;
    uint32_t t = toUnix(f[9], f[1]);
    if (t != 0) unix_time_ = t;
    return true;
  }

  // $--GGA,time,lat,N/S,lon,E/W,quality,sats,...
  bool parseGga(const char** f, int n) {
    if (n < 8) return false;
    int quality = atoiN(f[6]);
    sats_ = uint8_t(atoiN(f[7]));
    if (quality <= 0) { return false; }  // leave RMC as the fix authority
    return false;
  }

  static constexpr int32_t kInvalid = 0x7FFFFFFF;

  // Convert an NMEA "ddmm.mmmm" / "dddmm.mmmm" magnitude plus hemisphere into
  // signed micro-degrees. Degrees are the leading 2 (lat) or 3 (lon) digits;
  // the remainder is decimal minutes.
  static int32_t degMin(const char* s, char hemi) {
    if (!s || !s[0]) return kInvalid;
    // Locate the decimal point to know how many minute-integer digits precede it.
    int dot = -1;
    for (int i = 0; s[i]; i++) { if (s[i] == '.') { dot = i; break; } }
    if (dot < 4) return kInvalid;  // need at least dd + mm
    int deg_digits = dot - 2;      // 2 for latitude, 3 for longitude
    int64_t degrees = 0;
    for (int i = 0; i < deg_digits; i++) {
      if (s[i] < '0' || s[i] > '9') return kInvalid;
      degrees = degrees * 10 + (s[i] - '0');
    }
    // Minutes = the two integer digits before the dot plus the fractional part,
    // scaled to micro-minutes so we keep precision, then /60 to micro-degrees.
    // The first integer minute digit is the tens place: 10 minutes = 1e7 micro.
    int64_t min_micro = 0;      // minutes * 1e6
    int64_t scale = 10000000;   // weight of the tens-of-minutes digit
    for (int i = deg_digits; s[i]; i++) {
      if (s[i] == '.') continue;
      if (s[i] < '0' || s[i] > '9') break;
      min_micro += int64_t(s[i] - '0') * scale;
      scale /= 10;
      if (scale == 0) break;
    }
    int64_t micro = degrees * 1000000 + min_micro / 60;
    if (hemi == 'S' || hemi == 'W') micro = -micro;
    return int32_t(micro);
  }

  // Build a Unix timestamp from RMC date "ddmmyy" and time "hhmmss.sss".
  static uint32_t toUnix(const char* date, const char* time) {
    if (!date || std::strlen(date) < 6 || !time || std::strlen(time) < 6) return 0;
    int d = two(date), mo = two(date + 2), y = two(date + 4) + 2000;
    int hh = two(time), mm = two(time + 2), ss = two(time + 4);
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    // Days since 1970-01-01 via a civil-from-days algorithm (Howard Hinnant).
    int yy = y - (mo <= 2 ? 1 : 0);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = unsigned(yy - era * 400);
    unsigned doy = unsigned((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = int64_t(era) * 146097 + int64_t(doe) - 719468;
    return uint32_t(days * 86400 + hh * 3600 + mm * 60 + ss);
  }

  static int two(const char* s) {
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return -1;
    return (s[0] - '0') * 10 + (s[1] - '0');
  }
  static int atoiN(const char* s) {
    int v = 0;
    if (!s) return 0;
    for (int i = 0; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
    return v;
  }

  char line_[kMaxLine] = {};
  int len_ = 0;
  bool collecting_ = false;
  bool has_fix_ = false;
  int32_t lat_e6_ = 0;
  int32_t lon_e6_ = 0;
  uint32_t unix_time_ = 0;
  uint8_t sats_ = 0;
};

}  // namespace corefw::gps
