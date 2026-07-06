# corefw architecture

corefw separates a **stable, conservative kernel** (mechanisms) from
**components and policies** (which define the resulting device). The kernel
moves slowly and reliably; boards, integrations and behaviours evolve
independently — ideally in external repositories — without forking the firmware.

> The kernel provides mechanisms. Components and policies define the device.

## Two halves

| Half | Language | Role |
| --- | --- | --- |
| **Tooling** (`corefw`) | Go | Parse, validate, resolve and generate. Host-side only. |
| **Firmware** (`firmware/`) | C++ | The radio kernel + component implementations that run on the device. |

The tooling never runs on the device; the firmware never parses YAML. The bridge
between them is **code generation**: a validated profile becomes a C++
composition root plus a PlatformIO project.

## Layers

```
Kernel            protocol runtime · radio scheduler · crypto · storage ·
                  event system · power coordinator · airtime enforcement
Hardware drivers  radio · PMU · GPS · display · sensors
Board packages    pins, buses, RF switch, limits — assemble drivers
System/role       repeater · companion · room-server · sensor · gateway
Behaviour policy  power/sleep · routing · advert timing · regional
Applications      chat UI · tracker · telemetry — consume stable services
Profiles          a complete, buildable composition of the above
```

Each **category is distinct on purpose** — they have different privileges. A
driver implements hardware but decides no policy. A board owns pins but assembles
drivers. A policy tunes shared mechanisms. A module provides a role. Collapsing
these into one undifferentiated "component" (as early ESPHome did) loses the
ability to reason about who is allowed to do what.

## The guarded boundary

Modules request work through stable services; they do **not** get the radio.

```cpp
ctx.mesh().send(pkt);           // allowed: kernel decides when it is safe to TX
ctx.power().scheduleWake(t);    // allowed: coordinator arbitrates sleep
// radio.transmitRaw(...)       // not available to modules
```

This matters because MeshCore has **network-wide** (airtime, duty cycle) and
**battery-wide** constraints an ordinary sensor-component model does not. The
kernel keeps ownership of radio scheduling and power arbitration.

### The radio scheduler (implemented & host-tested)

`firmware/kernel/runtime/` realises the guarded boundary concretely:

- **`Dispatcher`** implements `MeshService`. Modules call `send()`/`subscribe()`;
  it pumps received frames through the router, delivers local packets to
  subscribers, queues forwards, and drains the TX queue onto the radio only when
  the duty-cycle limiter permits. It owns *when* things go on air.
- **`FloodRouter`** is pure logic: on a fresh flood packet it appends the node's
  hash to the path, bumps the count and schedules a retransmit (lower priority
  the further a packet has travelled) — matching the reference `routeRecvPacket`.
- **`Dedup`** is a 160-entry ring of `SHA256(type‖payload)[:8]` hashes. Because
  the hash excludes the mutating path, echoes of a packet we already forwarded
  are suppressed — the same property the reference firmware relies on.
- **`Airtime`** computes LoRa time-on-air (Semtech formula) and enforces the
  per-sub-band off-time rule, so the kernel keeps the whole mesh within duty.

`runtime_test.cpp` drives all of this with a fake radio + virtual clock: a fresh
flood is delivered and re-broadcast with the node's hash appended; duplicates and
echoes are dropped; and a second transmission is held until the duty cycle
permits.

### Central power coordination

Modules submit *requirements*; the `PowerCoordinator` decides the schedule:

```
repeater:   keep radio up 20s        power.requireRadioUntil(t)
telemetry:  next sample in 10 min    power.scheduleWake(t2)
policy:     battery low, prefer sleep
→ coordinator: radio on 20s, then deep sleep, wake in 10 min
```

## Wire compatibility

`firmware/kernel/protocol/` reimplements the MeshCore V1 packet and advert
formats byte-for-byte (`Packet.h`, `AdvertData.h`) as portable host code. The
header bit-fields, little-endian integers, path-length encoding, transport-code
placement and advert layout all match the reference firmware, and
`protocol_test.cpp` pins them with exact-byte assertions. Crypto identity is
Ed25519 with the same signed-message construction
(`pub_key || timestamp || app_data`).

## The build pipeline (Go)

1. **profile** — parse YAML, apply `${var}` substitution (declarative only; no
   arbitrary expressions, to keep builds validatable and reproducible).
2. **source** — fetch external components from local paths or git, pinning each
   git source to an exact resolved commit.
3. **registry** — merge built-in (embedded) + external components; id collisions
   are hard errors so a source can't silently shadow another.
4. **schema** — validate each component's options: types, ranges, enums, units,
   defaults, cross-field rules. Unknown keys fail. Errors are human-readable and
   early — never a template instantiation deep inside a compiler.
5. **resolve** — expand `auto_load`, check required capabilities/services/
   components, detect conflicts, and run final cross-component validation
   (tx-power vs board limit, display availability, RAM budget).
6. **codegen** — merge build fragments (defines/sources/libs) from only the
   selected components, and emit `platformio.ini` + a C++ composition root that
   constructs and registers each object with the kernel.
7. **lock** — write `corefw.lock`: platform version, every component + origin,
   resolved git commits, and a hash of the effective configuration.

## Reproducibility

Git-loaded components are powerful but moving refs make builds non-reproducible.
`corefw.lock` records exact commits and a config hash, so it is always possible
to determine exactly what code a node is running. Production profiles should pin
sources to commit hashes; branches/tags are accepted but recorded as resolved
commits.

## Public API versioning

The kernel versions its interfaces independently (Board API, Module API, Policy
API, Radio Driver API, Storage API). Components declare the ranges they support:

```yaml
compatibility:
  platform: ">=0.1 <0.2"
  module_api: "^0.1"
```

## What we deliberately do *not* copy from ESPHome

- **Arbitrary C++ lambdas in configuration** — weakens validation and
  reproducibility. Config stays declarative.
- **One undifferentiated component category** — drivers, boards, policies, roles
  and apps have different responsibilities and privileges.
- **Hidden moving dependencies** — production builds pin exact revisions.
- **"Every buildable combo is supported"** — we distinguish *buildable*,
  *tested* and *certified*.
- **Modules bypassing radio/power coordination** — the network- and
  battery-wide constraints are enforced by the kernel.
