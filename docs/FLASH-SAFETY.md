# Flash safety: reflashing a MeshCore device with corefw

A corefw image built for the **Wio Tracker L1** is safe to flash onto a device
already running the stock MeshCore firmware. It will not brick the device and it
does not destroy the node's stored identity, preferences, contacts or channels.
This document explains why, including the companion QSPI storage layout.

## No brick

corefw builds against the **identical** board definition MeshCore uses. The
board JSON (`components/boards/wio-tracker-l1/pio-board/seeed-wio-tracker-l1.json`)
is byte-for-byte the same as MeshCore's `boards/seeed-wio-tracker-l1.json`:

| Region              | Address                | Written by an app flash? |
|---------------------|------------------------|--------------------------|
| MBR + SoftDevice S140 7.3.0 | `0x00000 – 0x27000` | No |
| **Application (corefw companion)** | `0x27000 – 0xD4000` | **Yes** |
| Reserved gap / extra-fs boundary   | `0xD4000 – 0xED000` | No |
| Adafruit InternalFS (LittleFS) | `0xED000 – 0xF4000` | No |
| Bootloader + settings          | `0xF4000 – 0x100000` | No |

Flashing over USB uses the Adafruit UF2 / `nrfutil` DFU path, which writes **only
the application region**. The MBR, SoftDevice and bootloader are untouched, so
the device always retains a working bootloader — it can be re-flashed with any
firmware afterward. The SoftDevice version (S140 7.3.0) matches, so the BLE stack
the app links against is the one already on the device.

## Storage is preserved

corefw reads and writes the **same on-flash files, in the same byte formats**, as
MeshCore's `DataStore` (see `firmware/kernel/companion/StorageCodec.h`, verified
byte-for-byte by `tests/cpp/storage_test.cpp`):

- InternalFS `/_main.id` — `prv_key(64) || pub_key(32) || name(32)`
- InternalFS `/new_prefs` — 137-byte preferences record
- QSPI LittleFS `/contacts3` — 152 bytes per contact
- QSPI LittleFS `/channels2` — 68 bytes per channel

`NRF52FileStore::begin()` mounts InternalFS and the external P25Q16H QSPI
LittleFS used by MeshCore's Wio companion builds (`-D QSPIFLASH=1`,
`nrf52840_s140_v7_extrafs.ld`, `board_upload.maximum_size = 708608`). The Wio
board component uses that same linker script and size cap, so app flashing stops
before the InternalFS region. The node reappears on the mesh as the same node.
Only an explicit `CMD_FACTORY_RESET` from the companion app erases anything.

## Layout migration

On boot, corefw applies the same primary/secondary split MeshCore expects:

- If `/contacts3`, `/channels2` or `/adv_blobs` are found on InternalFS and the
  QSPI copy is missing, they are copied to QSPI and the misplaced InternalFS
  copy is removed.
- If `/_main.id` or `/new_prefs` are found on QSPI, they are copied back to
  InternalFS and the misplaced QSPI copy is removed.
- Old corefw MVP identity files at `/identity/_main.id` are moved once to
  MeshCore's nRF path, `/_main.id`.
- If QSPI cannot be mounted, corefw leaves the InternalFS copies in place and
  falls back to InternalFS for contacts/channels so the device remains usable.
