# cz-advert-features

Advertise the two 16-bit **advert feature bitmasks** (`feat1`/`feat2`) that
[meshcore-cz](https://github.com/meshcore-dev/MeshCore/compare/main...meshcore-cz:MeshCoreCZ:cz-advert-features)
added — but as a drop-in extension instead of a fork.

## Why this isn't a fork

The upstream branch forked the entire firmware to add the feature. Broken down,
it did three things:

| meshcore-cz change | corefw equivalent |
|--------------------|-------------------|
| Added `feat1`/`feat2` to the advert app_data wire format | **Already present** — `proto::AdvertData::feat1/feat2` + `ADV_FEAT1/FEAT2` bits ([`AdvertData.h`](../../../firmware/kernel/include/corefw/protocol/AdvertData.h)) |
| `buildAdvertData()` stamps the flags in | `CzAdvertFeatures::decorateAdvert()` — the kernel calls it on the advert before signing, via `Kernel::applyAdvertDecorators` |
| Persisted flags in a `/cz_advert` sidecar + `set advert.features` CLI | Set declaratively in the profile, resolved at build time (no NVS sidecar, no runtime command) |

Because the wire format and the hook already exist in the platform, the whole
feature is ~15 lines of header plus a manifest. Nothing in the kernel or the
repeater/companion modules is touched, and everything — manifest, schema, C++,
this README — lives in **this one directory**, so it is git-linkable as-is.

## Usage

```yaml
modules:
  - type: repeater
    advert_name: "CoreFW CZ Repeater"
  - type: cz-advert-features
    feat1: 0x0001     # decimal or 0x hex
    feat2: 0x00A0     # conventionally only set when feat1 is set
```

A ready profile: [`profiles/heltec-v3-repeater-cz.yaml`](../../../profiles/heltec-v3-repeater-cz.yaml).

## Options

| Option | Type | Range | Default | Meaning |
|--------|------|-------|---------|---------|
| `feat1` | integer | 0–65535 | 0 | Feature bitmask #1. `0` omits the field (wire-compatible with plain nodes). |
| `feat2` | integer | 0–65535 | 0 | Feature bitmask #2. `0` omits the field. |

## Runtime `set advert.features`

Beyond the build-time defaults, the flags are also settable at runtime over the
companion protocol, via the standard custom-var commands — no new command codes:

- `CMD_SET_CUSTOM_VAR advert.feat1 0x00ff` → updates `feat1` (decimal or hex)
- `CMD_GET_CUSTOM_VARS` → `advert.feat1:00ff,advert.feat2:00a0`

The next self-advert reflects the change. This works because the extension
implements `Module::setConfigVar`/`getConfigVars`, which the kernel fans out from
the companion custom-var path (`Kernel::setConfigVar`/`getConfigVars`).

**Not yet persisted.** A runtime change lives in RAM and resets on reboot (the
build-time `feat1`/`feat2` are the power-on defaults). To persist like upstream's
`/cz_advert` sidecar, extend this component to write through
`companion::PersistentStore` — still a change to *this* extension, not a fork.

## Verified

`tests/cpp/extension_test.cpp` (run via `make test`) proves the hook stamps
`feat1`/`feat2` onto an advert, the `ADV_FEAT1/FEAT2` bits are set on the wire,
the values round-trip through decode, and a zero-valued extension omits the
fields.
