# corefw

**An ESPHome-style component & build platform for MeshCore firmware.**

corefw turns MeshCore firmware from a monolith you fork into a **modular platform
you compose**. You describe a device declaratively in YAML — a board, some
modules, a few policies — and the `corefw` tool resolves, validates and
generates a statically-compiled firmware image against a shared, conservative
kernel.

```
declarative profile (YAML)
        ↓  parse + ${var} substitution
        ↓  fetch external components (local / git)
        ↓  schema validation (types, ranges, rules)
        ↓  dependency / conflict / capability resolution
        ↓  C++ code generation
        ↓  PlatformIO build
one device-specific firmware image  +  corefw.lock
```

The firmware kernel is **C++**, reimplemented with a modern modular architecture
but **wire-compatible with MeshCore Core Protocol V1** — corefw nodes
interoperate on the same mesh as existing MeshCore firmware. The tooling
(`corefw`) is **Go**.

> Status: MVP. Two official boards (Heltec V3, Wio Tracker L1), repeater +
> companion modules, one power policy, local + git external components,
> reproducible lockfiles.

---

## Quick start

```console
$ make build                 # build the corefw CLI
$ ./corefw components         # list built-in boards / modules / policies
$ ./corefw validate profiles/heltec-v3-repeater.yaml

# build compiles the firmware with PlatformIO by default:
$ ./corefw build  profiles/heltec-v3-repeater.yaml
$ ./corefw build  profiles/heltec-v3-repeater.yaml --no-compile   # just generate

# flash builds and uploads to a connected device:
$ ./corefw flash  profiles/heltec-v3-repeater.yaml --port /dev/ttyUSB0
```

If PlatformIO isn't installed, `build` still generates the project and prints the
manual `pio run` command instead of failing.

A profile is small and readable:

```yaml
name: heltec-v3-repeater
board: heltec-v3

modules:
  - type: repeater
    advert_name: "CoreFW Repeater CZ"
    max_neighbours: 80
  - type: companion
    transport: usb

policies:
  power:
    type: simple-power
    low_battery: 30
    critical_battery: 15
    tx_power: 22

radio:
  region: eu868
  freq: 869.525
```

## Commands

| Command | Purpose |
| --- | --- |
| `corefw build <profile>` | Resolve, generate and compile a firmware image (`--no-compile` to skip) |
| `corefw flash <profile>` | Build and upload to a connected device (`--port` optional) |
| `corefw validate <profile>` | Validate a profile + its component graph, no generation |
| `corefw components` / `corefw boards` | List available components |
| `corefw lock <profile>` | Print the resolved lockfile |
| `corefw version` | Print the platform version |

## Repository layout

```
cmd/corefw/           CLI entrypoint
internal/             the Go tooling
  profile/            profile YAML + ${var} substitution
  schema/             strict option validation (+ durations)
  manifest/           component manifest model
  registry/           built-in + external component discovery
  source/             local & git external-component fetching
  resolve/            dependency/conflict/capability/final validation
  codegen/            PlatformIO project + C++ composition-root generation
  lock/               corefw.lock reproducibility record
  build/              pipeline orchestration
components/           built-in ("official") components (embedded in the binary)
  boards/             heltec-v3, wio-tracker-l1
  modules/            repeater, companion
  policies/           simple-power
firmware/             the C++ kernel & component implementations
  kernel/             mechanisms: protocol, events, power, Kernel API
    protocol/         wire-compatible Packet, AdvertData, Identity, Advert, PacketHash
    runtime/          Dispatcher (radio scheduler), FloodRouter, Dedup, Airtime, Clock
    companion/        Companion Protocol frame codec + command/response/push codes
    include/corefw/   the public Board/Module/Policy/Mesh/Radio/Kernel API
  drivers/
    crypto/ed25519/   vendored orlp/ed25519 (same lib as MeshCore) — see its LICENSE
    crypto/sha256/    packet-hash digest (matches Core Protocol packet hash)
    radio/sx1262/     SX1262 RadioDriver (target-only, RadioLib)
profiles/             example build profiles
examples/             an example external component
docs/                 architecture & how-to guides
```

## Testing

```console
$ make test        # Go unit tests + host-side C++ wire & kernel tests
$ make verify-gen  # generate both example profiles and compile the C++ roots
```

The C++ protocol tests assert **exact byte layouts** against MeshCore V1, so any
change that would break mesh interoperability fails on a workstation before it
ever reaches a radio. The identity tests go further and reproduce MeshCore's own
embedded X25519 key-exchange test vector, proving the Ed25519/ECDH crypto is
byte-compatible (corefw vendors the same `orlp/ed25519` library). The companion
tests verify the Companion Protocol framing (`>`/`<` + LE16 length) and the full
command/response/push code set match the reference companion firmware, so
existing MeshCore phone apps connect unchanged.

**Protocol compatibility, verified on the host:**

| Layer | What matches MeshCore | Test |
| --- | --- | --- |
| Packet wire format | header bits, path encoding, transport codes | `protocol_test.cpp` |
| Advert | payload + signed-message layout | `identity_test.cpp` |
| Identity / ECDH | Ed25519 + X25519 (same `orlp/ed25519`) | `identity_test.cpp` (MeshCore vector) |
| Packet hash / dedup | `SHA256(type‖payload)[:8]` | `runtime_test.cpp` |
| Repeater (flood) | append-path forward, echo suppression, duty cycle | `runtime_test.cpp` |
| Companion | `>`/`<` framing + all CMD/RESP/PUSH codes | `companion_test.cpp` |

## Design

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the layering, the public
API boundaries, and what is deliberately *not* copied from ESPHome. See
[docs/adding-a-component.md](docs/adding-a-component.md) to publish a board or
policy without forking the platform.

## License

MIT.
