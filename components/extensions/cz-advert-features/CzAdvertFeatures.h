// CzAdvertFeatures — optional extension: stamp advert feature bitmasks.
//
// This is the corefw-native equivalent of the meshcore-cz "cz-advert-features"
// fork. Upstream forked the entire firmware to add two persistent 16-bit advert
// feature flags plus a `set advert.features <feat1> <feat2>` CLI command and a
// `/cz_advert` storage sidecar. In corefw the advert wire format already carries
// these fields (proto::AdvertData::feat1/feat2, ADV_FEAT1/FEAT2 bits), so the
// whole feature collapses to a self-contained extension component: it hooks the
// advert-build path through Module::decorateAdvert and stamps the flags in. No
// fork, no edits to the kernel or the repeater/companion modules.
//
// Drop it into a profile's module list (or pull it from its own git repo via
// external_components) and set feat1/feat2 there:
//
//   modules:
//     - type: cz-advert-features
//       feat1: 0x0001
//       feat2: 0x00A0
#pragma once

#include <corefw/Module.h>
#include <corefw/protocol/AdvertData.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace corefw {

class CzAdvertFeatures : public Module {
 public:
  const char* name() const override { return "cz-advert-features"; }

  // Config setters, driven by the generated composition code from validated
  // profile options.
  void setFeat1(int f) { feat1_ = uint16_t(f); }
  void setFeat2(int f) { feat2_ = uint16_t(f); }

  // Extension hook: stamp the flags onto every outgoing self-advert. A value of
  // 0 is left untouched, so the field stays omitted — matching the upstream
  // default and keeping the advert byte-compatible with plain nodes.
  void decorateAdvert(proto::AdvertData& ad) override {
    if (feat1_) ad.feat1 = feat1_;
    if (feat2_) ad.feat2 = feat2_;
  }

  // Extension hooks: expose the flags over the companion custom-var command
  // surface, so a paired app can read/change them at runtime — the corefw
  // equivalent of `set/get advert.features`, over the existing protocol. The
  // new value is picked up by the next self-advert via decorateAdvert().
  bool setConfigVar(const char* name, const char* value) override {
    if (std::strcmp(name, "advert.feat1") == 0) { feat1_ = parseU16(value); return true; }
    if (std::strcmp(name, "advert.feat2") == 0) { feat2_ = parseU16(value); return true; }
    return false;
  }
  size_t getConfigVars(char* out, size_t cap) override {
    // "advert.feat1:XXXX,advert.feat2:YYYY" in hex, matching upstream's format.
    int w = std::snprintf(out, cap, "advert.feat1:%04x,advert.feat2:%04x", feat1_, feat2_);
    if (w < 0) return 0;
    return (size_t(w) < cap) ? size_t(w) : cap;
  }

  uint16_t feat1() const { return feat1_; }
  uint16_t feat2() const { return feat2_; }

 private:
  // Parse a 16-bit value in decimal or 0x-prefixed hex (like upstream parseUInt16).
  static uint16_t parseU16(const char* s) {
    if (!s) return 0;
    return uint16_t(std::strtoul(s, nullptr, 0) & 0xFFFF);
  }

  uint16_t feat1_ = 0;
  uint16_t feat2_ = 0;
};

}  // namespace corefw
