# Flash safety: reflashing a MeshCore device with corefw

A corefw image built for the **Wio Tracker L1** is safe to flash onto a device
already running the stock MeshCore firmware. It will not brick the device and it
does not destroy the node's stored identity or preferences. This document
explains why, and the one caveat.

## No brick

corefw builds against the **identical** board definition MeshCore uses. The
board JSON (`components/boards/wio-tracker-l1/pio-board/seeed-wio-tracker-l1.json`)
is byte-for-byte the same as MeshCore's `boards/seeed-wio-tracker-l1.json`:

| Region              | Address                | Written by an app flash? |
|---------------------|------------------------|--------------------------|
| MBR + SoftDevice S140 7.3.0 | `0x00000 – 0x27000` | No |
| **Application (corefw)**    | `0x27000 – 0xED000` | **Yes** |
| Adafruit InternalFS (LittleFS) | `0xED000 – 0xF4000` | No |
| Bootloader + settings          | `0xF4000 – 0x100000` | No |

Flashing over USB uses the Adafruit UF2 / `nrfutil` DFU path, which writes **only
the application region**. The MBR, SoftDevice and bootloader are untouched, so
the device always retains a working bootloader — it can be re-flashed with any
firmware afterward. The SoftDevice version (S140 7.3.0) matches, so the BLE stack
the app links against is the one already on the device.

## Storage is preserved, never formatted

corefw reads and writes the **same on-flash files, in the same byte formats**, as
MeshCore's `DataStore` (see `firmware/kernel/companion/StorageCodec.h`, verified
byte-for-byte by `tests/cpp/storage_test.cpp`):

- `/identity/_main.id` — `prv_key(64) || pub_key(32) || name(32)`
- `/new_prefs` — 137-byte preferences record
- `/contacts3` — 152 bytes per contact
- `/channels2` — 68 bytes per channel

`NRF52FileStore::begin()` **mounts** the existing InternalFS; it never formats.
Because InternalFS lives at a fixed address below the bootloader (independent of
the application's linker script), a device reflashed from MeshCore keeps its
**identity** — and therefore its mesh address / public key — and its
**preferences**. The node reappears on the mesh as the same node. Only an
explicit `CMD_FACTORY_RESET` from the companion app erases anything.

## Caveat: contacts/channels on the QSPI-flash companion layout

The stock MeshCore *companion* build for this board (`WioTrackerL1_companion_radio_*`)
stores `/contacts3` and `/channels2` on the **external QSPI flash** (`-D QSPIFLASH=1`,
`nrf52840_s140_v7_extrafs.ld`). corefw's MVP keeps all four files on the internal
LittleFS. Consequences:

- **Identity and preferences transfer** (both are on InternalFS in every layout).
- **Contacts and channels may not transfer** on the first boot after reflashing
  from a MeshCore companion image, because corefw looks for them on InternalFS
  rather than QSPI. This is non-destructive: nothing is erased, the QSPI copy
  remains, and the companion app re-syncs contacts. Reading the QSPI store is a
  planned follow-up (needs the external-flash driver).

Reflashing from a MeshCore **repeater** image (which uses InternalFS for
everything) transfers contacts and channels too.
