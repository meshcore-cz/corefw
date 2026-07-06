// gps_test — verifies the portable NMEA decoder against known sentences.
#include <drivers/gps/NmeaParser.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using corefw::gps::NmeaParser;

static int failures = 0;
static void check(bool cond, const char* what) {
  if (!cond) { std::printf("FAIL: %s\n", what); failures++; }
}

// Feed a whole sentence (parser resets on '$', parses on newline).
static bool feedLine(NmeaParser& p, const char* s) {
  bool parsed = p.feed('$');
  for (const char* c = s; *c; c++) parsed |= p.feed(*c);
  parsed |= p.feed('\n');
  return parsed;
}

static bool near(int32_t got, int32_t want, int32_t tol) {
  int32_t d = got - want;
  if (d < 0) d = -d;
  return d <= tol;
}

int main() {
  // A valid RMC: 48deg 07.038' N, 11deg 31.000' E on 2023-03-23 12:35:19 UTC.
  {
    NmeaParser p;
    bool got = feedLine(
        p, "GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230323,003.1,W*6A");
    check(got, "rmc parsed");
    check(p.hasFix(), "rmc has fix");
    // 48 + 7.038/60 = 48.1173 deg -> 48117300 ue (tolerance for /60 rounding).
    check(near(p.latE6(), 48117300, 50), "rmc latitude");
    // 11 + 31.0/60 = 11.516667 deg -> 11516666 ue.
    check(near(p.lonE6(), 11516666, 50), "rmc longitude");
    // 2023-03-23 12:35:19 UTC = 1679574919.
    check(p.unixTime() == 1679574919u, "rmc unix time");
  }

  // Southern/Western hemisphere signs.
  {
    NmeaParser p;
    feedLine(p, "GNRMC,000000,A,3352.000,S,15112.000,E,0,0,010120,,*00");
    check(p.latE6() < 0, "south latitude negative");
    check(p.lonE6() > 0, "east longitude positive");
    check(near(p.latE6(), -33866666, 50), "south lat value");
  }

  // Void RMC (status V) clears the fix.
  {
    NmeaParser p;
    feedLine(p, "GPRMC,123519,A,4807.038,N,01131.000,E,0,0,230323,,*00");
    check(p.hasFix(), "fix set before void");
    feedLine(p, "GPRMC,123520,V,,,,,,,230323,,*00");
    check(!p.hasFix(), "void rmc clears fix");
  }

  // GGA updates satellite count without asserting a fix on its own.
  {
    NmeaParser p;
    feedLine(p, "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    check(p.satellites() == 8, "gga satellites");
  }

  // Split reads (byte-at-a-time across the boundary) still parse.
  {
    NmeaParser p;
    const char* s = "$GPRMC,123519,A,4807.038,N,01131.000,E,0,0,230323,,*00\n";
    bool parsed = false;
    for (const char* c = s; *c; c++) parsed |= p.feed(*c);
    check(parsed, "streamed rmc parsed");
    check(p.hasFix(), "streamed rmc fix");
  }

  if (failures == 0) std::printf("all GPS NMEA parser tests passed\n");
  return failures ? 1 : 0;
}
