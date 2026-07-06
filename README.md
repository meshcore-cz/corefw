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
> reproducible lockfiles. The **Wio Tracker L1 companion** is functionally
> complete on the portable layer: all 65 Companion-Protocol commands, encrypted
> direct/group messaging (send **and** receive), contacts & channels, and
> MeshCore-compatible flash storage — see
> [Flashing over MeshCore](#flashing-over-meshcore) and
> [docs/FLASH-SAFETY.md](docs/FLASH-SAFETY.md). Not yet run on real hardware.

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

See [docs/CLI.md](docs/CLI.md) for the full command tree, global flags,
workflow examples, JSON output options and shell completion setup.

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
    protocol/         wire-compatible Packet, AdvertData, Identity, Advert, PacketHash,
                      MessageCrypto (AES-128 + HMAC), Datagram (direct/group/anon builders)
    runtime/          Dispatcher (radio scheduler), FloodRouter, Dedup, Airtime, Clock
    companion/        Companion Protocol: frame codec, all 65 CMD codes + RESP/PUSH,
                      command handler, contacts/channels/offline stores, RX decrypt
                      (Receiver), byte-compatible flash storage (StorageCodec/Storage)
    ui/               CompanionUI (OLED screen model) + RTTTL buzzer/melody sequencer
    include/corefw/   the public Board/Module/Policy/Mesh/Radio/Kernel API
  drivers/
    crypto/ed25519/   vendored orlp/ed25519 (same lib as MeshCore) — see its LICENSE
    crypto/sha256/    packet-hash + HMAC digest (matches Core Protocol packet hash)
    crypto/aes/       AES-128 block cipher (message-payload encryption, FIPS-197)
    radio/sx1262/     SX1262 RadioDriver (target-only, RadioLib)
    display/sh1106/   SH1106 OLED driver (target-only, Adafruit)
    buzzer/           Arduino tone() buzzer output (target-only)
  boards/wio_tracker_l1/  BLE transport + InternalFS store + companion entrypoint (target-only)
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
ever reaches a radio. Several tests go further and reproduce MeshCore's *own*
embedded/standard vectors: the X25519 key-exchange vector (proving Ed25519/ECDH
is byte-compatible — corefw vendors the same `orlp/ed25519`), the FIPS-197 AES
vector and the RFC 4231 HMAC vector (message encryption), and the storage tests
pin every record field offset so a corefw flash reads an existing device's data
correctly.

**Protocol & app compatibility, verified on the host (11 suites):**

| Layer | What matches MeshCore | Test |
| --- | --- | --- |
| Packet wire format | header bits, path encoding, transport codes | `protocol_test.cpp` |
| Advert | payload + signed-message layout | `identity_test.cpp` |
| Identity / ECDH | Ed25519 + X25519 (same `orlp/ed25519`) | `identity_test.cpp` (MeshCore vector) |
| Packet hash / dedup | `SHA256(type‖payload)[:8]` | `runtime_test.cpp` |
| Repeater (flood) | append-path forward, echo suppression, duty cycle | `runtime_test.cpp` |
| Message crypto | AES-128 + `encryptThenMAC`/`MACThenDecrypt` | `crypto_msg_test.cpp` (FIPS-197, RFC 4231) |
| Datagram builders | direct / group / anon layout + plaintext | `datagram_test.cpp` (ECDH round-trip) |
| Companion framing | `>`/`<` + LE16 length, split/resync | `companion_test.cpp` |
| Companion commands | **all 65 CMD codes** — config, contacts, channels, messaging, requests, signing | `commands_test.cpp` |
| Receive path | decrypt inbound direct/group msgs + verify adverts | `receiver_test.cpp` (A→B interop) |
| Flash storage | identity / prefs / contacts / channels record layouts | `storage_test.cpp` (offset-pinned) |
| Companion UI + beeps | OLED screen model, RTTTL melody sequencing | `ui_test.cpp` |

## Flashing over MeshCore

A corefw Wio Tracker L1 image is **safe to flash onto a device already running
stock MeshCore** — it will not brick the device and it preserves the node's
stored identity and preferences:

- **No brick.** corefw builds against the *byte-identical* board definition
  MeshCore uses (same SoftDevice S140 7.3.0, same bootloader). A USB flash writes
  only the application region; the MBR, SoftDevice and bootloader are untouched.
- **Data preserved.** corefw reads/writes the same on-flash files in the same
  byte formats as MeshCore's `DataStore` (`/_main.id`, `/new_prefs`,
  `/contacts3`, `/channels2`). On Wio Tracker L1 companion builds, identity and
  prefs stay on InternalFS while contacts/channels use the external QSPI flash,
  matching MeshCore's `QSPIFLASH` layout. Only an explicit factory-reset command
  erases anything.

Full details, the flash memory map, and the storage migration behavior are in
[docs/FLASH-SAFETY.md](docs/FLASH-SAFETY.md).

## Design

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the layering, the public
API boundaries, and what is deliberately *not* copied from ESPHome. See
[docs/adding-a-component.md](docs/adding-a-component.md) to publish a board or
policy without forking the platform.

## License

MIT.
