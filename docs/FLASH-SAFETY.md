# Flash safety

**Flash safety is a corefw design default, not a per-board feature.** A corefw
image built for any supported board is safe to flash onto a device already
running stock MeshCore firmware: it will not brick the device, and it preserves
the node's stored identity, preferences, contacts and channels so the node
reappears on the mesh unchanged.

Every board component is expected to uphold the two guarantees below. When you
add a board (see [adding-a-component.md](adding-a-component.md)), treat them as
acceptance criteria, not afterthoughts.

## The two guarantees

1. **No brick.** Flashing writes only the application region(s). The immutable
   boot chain — bootloader, and on nRF52 the SoftDevice — is never overwritten,
   so the device can always be re-flashed with any firmware afterward.

2. **Data is preserved.** corefw reads and writes the **same on-flash files, in
   the same byte formats, on the same filesystem, at the same flash offset** as
   MeshCore's `DataStore`. The persistent data partition is outside the region an
   app flash writes, so it survives; because the filesystem and record layouts
   match, corefw mounts the existing volume instead of reformatting it. Only an
   explicit `CMD_FACTORY_RESET` from the companion app erases anything.

## How the guarantees are met

**No brick — same boot chain.** corefw builds against the *identical* board
definition MeshCore uses, so the flash memory map is the same and the upload path
writes only application region(s):

- **nRF52 (e.g. Wio Tracker L1):** UF2 / `nrfutil` DFU writes only the
  application region. The MBR, SoftDevice (S140 7.3.0) and bootloader are
  untouched. The SoftDevice version matches, so the BLE stack the app links
  against is the one already on the device.
- **ESP32-S3 (e.g. Heltec V3):** `esptool` writes the bootloader, partition
  table and app image. corefw uses the same board (`esp32-s3-devkitc-1`) and the
  same default partition table as MeshCore, so the bootloader/partition-table
  writes are byte-identical rewrites of what is already there, and the ROM
  bootloader in the SoC is never touched — the device stays recoverable.

**Data preserved — byte-compatible storage.** The record formats are verified
byte-for-byte by `tests/cpp/storage_test.cpp` against
`firmware/kernel/companion/StorageCodec.h`:

| File | Format |
|------|--------|
| `/_main.id`    | `prv_key(64) ‖ pub_key(32) ‖ name(32)` |
| `/new_prefs`   | 137-byte preferences record |
| `/contacts3`   | 152 bytes per contact |
| `/channels2`   | 68 bytes per channel |

The *filesystem* carrying those files, and where it lives, is board-specific —
covered per architecture below.

## nRF52 (Wio Tracker L1)

Flash map (`components/boards/wio-tracker-l1/pio-board/seeed-wio-tracker-l1.json`
is byte-for-byte MeshCore's `boards/seeed-wio-tracker-l1.json`):

| Region | Address | Written by an app flash? |
|--------|---------|--------------------------|
| MBR + SoftDevice S140 7.3.0 | `0x00000 – 0x27000` | No |
| **Application (corefw companion)** | `0x27000 – 0xD4000` | **Yes** |
| Reserved gap / extra-fs boundary   | `0xD4000 – 0xED000` | No |
| Adafruit InternalFS (LittleFS) | `0xED000 – 0xF4000` | No |
| Bootloader + settings          | `0xF4000 – 0x100000` | No |

- **Identity + prefs** live on the internal LittleFS (`InternalFS`), which sits
  above the application region and is never written by an app flash.
- **Contacts + channels** live on the **external P25Q16H QSPI flash** on stock
  Wio companion builds (`-D QSPIFLASH=1`). corefw uses the same QSPI store
  (`NRF52FileStore.h` + `NRF52QSPIFileSystem.h`), gated on in
  `components/boards/wio-tracker-l1/component.yaml`.
- **Layout migration** on boot mirrors MeshCore's primary/secondary split:
  `/contacts3`, `/channels2`, `/adv_blobs` are moved to QSPI if found on
  InternalFS with no QSPI copy; `/_main.id` and `/new_prefs` are moved back to
  InternalFS if found on QSPI; the QSPI mount is non-destructive (never
  auto-formats). If QSPI can't be mounted, corefw falls back to InternalFS for
  contacts/channels so the device stays usable.

## ESP32-S3 (Heltec V3)

No external flash — identity, prefs, contacts and channels all live in one
internal filesystem, so there is no dual-FS routing or QSPI mount. Two facts make
a reflash from stock MeshCore data-safe:

1. **Same partition table.** Both corefw and MeshCore build for
   `esp32-s3-devkitc-1` and neither overrides `board_build.partitions`, so both
   use the framework's `default_8MB.csv`. The data partition (`spiffs`, subtype
   `spiffs`, `0x670000 – 0x7F0000`) is at the same offset in both, and an
   `esptool` app upload (bootloader `0x0`, partitions `0x8000`, `boot_app0`
   `0xe000`, app `0x10000`) does not touch it.

2. **Same filesystem — SPIFFS, deliberately.** MeshCore's ESP32 companion stores
   its data on **SPIFFS** (`examples/companion_radio/main.cpp` → `SPIFFS.begin(true)`),
   *not* LittleFS. So `boards/heltec_v3/ESP32FileStore.h` uses `SPIFFS` too. This
   matters: mounting MeshCore's SPIFFS volume with a LittleFS driver would fail
   the mount and (with format-on-fail) **reformat the partition, destroying the
   node identity**. Matching the filesystem is what upholds guarantee #2 on ESP32.

   > Trade-off: SPIFFS is deprecated in ESP-IDF in favour of LittleFS. corefw
   > uses it here for MeshCore on-flash compatibility, consistent with the
   > project's byte-for-byte storage philosophy. If a board is *not* expected to
   > inherit stock-MeshCore data (a corefw-only device), LittleFS is the better
   > choice — but then it must be used consistently from that board's first flash.

`SPIFFS.begin(true)` formats only a **blank** partition (a brand-new device);
an existing MeshCore or corefw SPIFFS volume mounts as-is and its data is read
through the byte-compatible codecs above.

## Adding a board: the flash-safety checklist

- Use the *same* PlatformIO board definition / flash map as MeshCore for that
  hardware, so the upload path writes only application region(s).
- Store the four records at the MeshCore paths (`/_main.id`, `/new_prefs`,
  `/contacts3`, `/channels2`) via `StorageCodec.h` — never invent a new layout.
- Mount the *same filesystem MeshCore uses on that hardware* on the *same
  partition/region*. Match, don't modernise, unless the board is corefw-only.
- Never auto-format an existing volume. Format-on-fail is acceptable **only** to
  initialise a blank partition on a fresh device (matching MeshCore's own
  behaviour); it must never fire on a populated volume.
- Erasing stored data is reserved for explicit `CMD_FACTORY_RESET`.
