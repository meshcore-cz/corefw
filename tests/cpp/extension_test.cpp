// Host-side test for the advert-decorator extension hook.
//
// Proves that an optional extension component (CzAdvertFeatures) can enrich a
// self-advert through Module::decorateAdvert / Kernel::applyAdvertDecorators
// without any core changes, and that the resulting advert round-trips on the
// wire with the FEAT1/FEAT2 bits set. Build & run:
//
//   c++ -std=c++17 -I firmware/kernel/include \
//       tests/cpp/extension_test.cpp -o /tmp/exttest && /tmp/exttest
#include <corefw/Kernel.h>
#include <corefw/extensions/CzAdvertFeatures.h>
#include <corefw/protocol/AdvertData.h>

#include <cstdio>
#include <cstdlib>

using namespace corefw;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

// A plain module must leave the advert untouched — the hook is opt-in.
namespace {
class PlainModule : public Module {
 public:
  const char* name() const override { return "plain"; }
};
}  // namespace

int main() {
  Kernel kernel;

  PlainModule plain;
  CzAdvertFeatures cz;
  cz.setFeat1(0x0001);
  cz.setFeat2(0x00A0);

  kernel.registerModule(&plain);
  kernel.registerModule(&cz);

  // Build a base advert the way the platform's sendAdvert() does, then apply
  // decorators before signing.
  proto::AdvertData ad;
  ad.type = proto::ADV_TYPE_REPEATER;
  std::strncpy(ad.name, "CoreFW CZ", sizeof(ad.name) - 1);

  check(ad.feat1 == 0 && ad.feat2 == 0, "feats start unset");
  kernel.applyAdvertDecorators(ad);
  check(ad.feat1 == 0x0001, "extension stamped feat1");
  check(ad.feat2 == 0x00A0, "extension stamped feat2");

  // Round-trip through the wire encoding: the FEAT bits must be set and the
  // values must survive decode, so existing MeshCore nodes read them back.
  uint8_t blob[proto::MAX_ADVERT_DATA_SIZE];
  uint8_t n = ad.encode(blob);
  check((blob[0] & proto::ADV_FEAT1_MASK) != 0, "ADV_FEAT1 bit set on wire");
  check((blob[0] & proto::ADV_FEAT2_MASK) != 0, "ADV_FEAT2 bit set on wire");

  proto::AdvertData decoded;
  check(decoded.decode(blob, n), "advert decodes");
  check(decoded.feat1 == 0x0001, "feat1 survives round-trip");
  check(decoded.feat2 == 0x00A0, "feat2 survives round-trip");
  check(std::strcmp(decoded.name, "CoreFW CZ") == 0, "name survives round-trip");

  // A zero-valued extension leaves the fields omitted (wire-compatible default).
  CzAdvertFeatures off;
  Kernel k2;
  k2.registerModule(&off);
  proto::AdvertData bare;
  bare.type = proto::ADV_TYPE_CHAT;
  k2.applyAdvertDecorators(bare);
  check(bare.feat1 == 0 && bare.feat2 == 0, "zero extension omits feats");

  std::printf("advert extension test passed\n");
  return 0;
}
