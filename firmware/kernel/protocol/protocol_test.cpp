// Host-side wire-compatibility tests for the corefw protocol layer.
//
// These assert exact byte layouts against the MeshCore V1 Core Protocol, so a
// regression that would break interoperability with existing firmware fails
// here on a workstation, long before it reaches a radio. Build & run:
//
//   c++ -std=c++17 -I firmware/kernel/include \
//       firmware/kernel/protocol/protocol_test.cpp -o /tmp/ptest && /tmp/ptest
#include <corefw/protocol/AdvertData.h>
#include <corefw/protocol/Packet.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace corefw::proto;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

static void testAdvertName() {
  AdvertData a;
  a.type = ADV_TYPE_REPEATER;
  std::strcpy(a.name, "AB");
  uint8_t buf[MAX_ADVERT_DATA_SIZE];
  uint8_t n = a.encode(buf);
  // flags = type(2) | NAME(0x80) = 0x82, then 'A','B'
  check(n == 3, "advert name length");
  check(buf[0] == 0x82, "advert name flags");
  check(buf[1] == 'A' && buf[2] == 'B', "advert name bytes");

  AdvertData b;
  check(b.decode(buf, n), "advert name decode");
  check(b.type == ADV_TYPE_REPEATER, "advert decoded type");
  check(std::strcmp(b.name, "AB") == 0, "advert decoded name");
}

static void testAdvertLatLon() {
  AdvertData a;
  a.type = ADV_TYPE_CHAT;
  a.setLatLon(50.0, 14.0);  // lat=50_000_000, lon=14_000_000
  std::strcpy(a.name, "N");
  uint8_t buf[MAX_ADVERT_DATA_SIZE];
  uint8_t n = a.encode(buf);
  // flags = 1 | LATLON(0x10) | NAME(0x80) = 0x91
  check(buf[0] == 0x91, "latlon flags");
  // lat 50_000_000 = 0x02FAF080 little-endian
  check(buf[1] == 0x80 && buf[2] == 0xF0 && buf[3] == 0xFA && buf[4] == 0x02, "lat LE");
  // lon 14_000_000 = 0x00D59F80 little-endian
  check(buf[5] == 0x80 && buf[6] == 0x9F && buf[7] == 0xD5 && buf[8] == 0x00, "lon LE");
  check(buf[9] == 'N', "name after latlon");

  AdvertData b;
  check(b.decode(buf, n), "latlon decode");
  check(b.lat == 50000000 && b.lon == 14000000, "latlon values");
  check(b.has_loc, "has_loc");
  check(std::strcmp(b.name, "N") == 0, "name value");
}

static void testPacketRoundTrip() {
  Packet p;
  p.setRouteAndType(ROUTE_FLOOD, PAYLOAD_ADVERT);
  check(p.routeType() == ROUTE_FLOOD, "route type");
  check(p.payloadType() == PAYLOAD_ADVERT, "payload type");
  check(!p.hasTransportCodes(), "flood has no transport codes");

  // path: 2 hashes of size 1
  p.setPathHashSizeAndCount(1, 2);
  p.path[0] = 0xAA;
  p.path[1] = 0xBB;
  check(p.pathByteLen() == 2, "path byte len");

  const char* body = "hello-mesh";
  p.payload_len = uint16_t(std::strlen(body));
  std::memcpy(p.payload, body, p.payload_len);

  uint8_t wire[MAX_TRANS_UNIT];
  size_t n = p.writeTo(wire);
  check(n == p.rawLength(), "raw length matches writeTo");
  // header, path_len byte, path(2), payload(10) => 1+1+2+10 = 14
  check(n == 14, "expected wire size");
  check(wire[0] == p.header, "header byte");
  check(wire[1] == uint8_t(p.path_len), "path_len byte");
  check(wire[2] == 0xAA && wire[3] == 0xBB, "path bytes");

  Packet q;
  check(q.readFrom(wire, n), "readFrom");
  check(q.header == p.header, "rt header");
  check(q.pathByteLen() == 2 && q.path[0] == 0xAA && q.path[1] == 0xBB, "rt path");
  check(q.payload_len == p.payload_len, "rt payload len");
  check(std::memcmp(q.payload, p.payload, q.payload_len) == 0, "rt payload");
}

static void testTransportCodes() {
  Packet p;
  p.setRouteAndType(ROUTE_TRANSPORT_FLOOD, PAYLOAD_TXT_MSG);
  check(p.hasTransportCodes(), "transport flood has codes");
  p.transport_codes[0] = 0x1234;
  p.transport_codes[1] = 0xABCD;
  p.payload_len = 1;
  p.payload[0] = 0x7;
  uint8_t wire[MAX_TRANS_UNIT];
  size_t n = p.writeTo(wire);
  // header + 4 transport + path_len + 0 path + 1 payload = 7
  check(n == 7, "transport wire size");
  check(wire[1] == 0x34 && wire[2] == 0x12, "transport code 0 LE");
  check(wire[3] == 0xCD && wire[4] == 0xAB, "transport code 1 LE");
  Packet q;
  check(q.readFrom(wire, n), "transport readFrom");
  check(q.transport_codes[0] == 0x1234 && q.transport_codes[1] == 0xABCD, "rt transport codes");
}

int main() {
  testAdvertName();
  testAdvertLatLon();
  testPacketRoundTrip();
  testTransportCodes();
  std::printf("all protocol wire-compatibility tests passed\n");
  return 0;
}
