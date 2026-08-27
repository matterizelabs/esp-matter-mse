# Matter WS2812 Animation Light

A Matter **extended color light** on ESP32 that plays LED effects on a
**48-LED (8×6) WS2812 matrix**. A companion tool generates procedural effects as raw RGB
frames, which are streamed to the device over a **custom Matter vendor cluster**
(`0x1618FC01`), cached in flash, and played back at 30 fps. Interactive control (on/off,
brightness, color) comes from the standard Matter light clusters.

- **Vendor / Product:** Matterize Labs `0x1618` / `0x0001`
- **Target:** ESP32 (classic, no PSRAM), flashed via `/dev/ttyUSB0`
- **Toolchain:** esp-idf `v6.0.2`, esp-matter `release-v1.6`

## How it works

```
LED effect (solid / chase / rainbow / ...)
   └─ tools/effects.py   generate → 8×6 RGB frames → SHA-256 + hex chunks
        └─ chip-tool write-by-id (custom cluster 0x1618FC01)
             └─ ESP32: RAM jitter buffer → 30 fps playback → RMT → WS2812
                  └─ background flash cache (5 slots, LRU)
```

Transfer protocol (all writable attributes are octet strings, written as `hex:`):

1. `TransferHash`  (`0x0005`) - announce target animation (SHA-256)
2. `TransferMeta`  (`0x0006`) - `[total_frames u16][fps u8][loop u8][w u8][h u8]`
3. `FrameChunk`    (`0x0007`) - `[frame_index u16][count u8][w u8][h u8][fps u8][RGB…]`
4. `PlayCmd`       (`0x0009`) - `hex:01` PLAY, `hex:02` STOP, `hex:03` CLEAR_CACHE

Re-sending only `TransferHash` + `PlayCmd` for a cached hash replays from flash.

## Repository layout

```
CMakeLists.txt            ESP-IDF project (board path set here, no env var needed)
sdkconfig.defaults        project defaults
partitions.csv            app + factory (fctry) + anim_cache
main/                     app glue: app_main + app_driver + Kconfig.projbuild
components/
├── ws2812_matrix/        board HAL + LED driver + button + color math
└── animation/            flash cache + playback engine + wire codec + custom cluster
shared/
└── wire_contract.json    single source of truth for the wire protocol
tools/
├── generate_wire.py      wire_contract.json → anim_protocol.h + protocol.py
├── effects.py            procedural LED effects → paste-ready chip-tool commands
├── matter_anim/          effects / codec / cli / protocol
├── certs/                factory-partition (DAC/PAI/CD) tooling
└── tests/                pytest suite (incl. wire-contract conformance)
docs/
└── esp-matter-patches/   SDK patch required to build
```

## Prerequisites

- esp-idf `v6.0.2` at `~/.espressif/versions/esp-idf/v6.0.2`
- esp-matter `release-v1.6` at `~/.espressif/versions/esp-matter/release-v1.6`, **with the
  long-octet-string patch applied** - see `docs/esp-matter-patches/README.md`
- Python 3.12+ with `uv`

## Build & flash

```bash
export IDF_PATH=$HOME/.espressif/versions/esp-idf/v6.0.2
export ESP_MATTER_PATH=$HOME/.espressif/versions/esp-matter/release-v1.6
. $IDF_PATH/export.sh
. $ESP_MATTER_PATH/export.sh
export PATH="$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/.environment/cipd/packages/pigweed:$PATH"

idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

The board profile path is set in `CMakeLists.txt`, so `ESP_MATTER_DEVICE_PATH` is not required.

Hardware config is in `main/Kconfig.projbuild` (defaults):

| Option | Default |
|---|---|
| `CONFIG_WS2812_GPIO` | `3` |
| `CONFIG_MATRIX_WIDTH` / `_HEIGHT` | `8` / `6` |
| `CONFIG_MATRIX_SERPENTINE` | `y` |
| `CONFIG_ANIM_FPS` | `30` |

## Companion tool

```bash
cd tools
uv run python effects.py <effect> [options] --node-id 1 > /tmp/anim.sh
```

Generates a procedural LED effect as a paste-ready sequence of `chip-tool any write-by-id`
commands (hash → meta → chunks → play). Run each as `uv run python effects.py ...`:

| Effect | Example |
|---|---|
| `solid` | `effects.py solid --color ff8800 --seconds 2` |
| `chase` | `effects.py chase --color ff0000 --speed 15 --size 1 --tail 4` |
| `comet` | `effects.py comet --color 00aaff --speed 10 --tail 8` |
| `pulse` | `effects.py pulse --color ff8800 --period 2` |
| `rainbow` | `effects.py rainbow --seconds 6` (add `--spatial` for a gradient) |
| `sparkle` | `effects.py sparkle --color ffffff --density 0.3 --seed 7` |
| `wipe` | `effects.py wipe --color 00aaff --direction ltr` |
| `strobe` | `effects.py strobe --color ffffff --rate 4` |
| `fire` | `effects.py fire --seed 3` |

Common options: `--color RRGGBB`, `--seconds`, `--fps` (default 30), `--node-id` (default 1),
`--prefix` (default `./chip-tool`; use `ct` when piping to the chiptool host). The emitted
lines (minus the prefix) can also be pasted into `chip-tool interactive start`.

## Wire contract

`shared/wire_contract.json` is the single source of truth for the cluster ID, attribute IDs,
and byte layouts. Regenerate the C header and Python module with:

```bash
python3 tools/generate_wire.py
```

`tools/tests/test_wire_contract.py` regenerates-and-diffs to catch drift.

## Attestation

Device credentials (CD + PAI + DAC + commissioning data) live in the `fctry` partition and
are generated with `esp-matter-mfg-tool` - see `tools/certs/README.md`. Current dev material
uses the test PAA / test CD key.

Onboarding / setup:

| | Value |
|---|---|
| Setup code (manual pairing code) | `2048-915-4736` |
| QR code | `MT:UFEA08-E150QJ850Y10` |
| Discriminator | `2245` |
| Passcode | `89674523` |

The `fctry` partition must be flashed separately (it is not written by `idf.py flash`):

```bash
BIN=tools/certs/out/1618_1/<uuid>/<uuid>-partition.bin
esptool.py --port /dev/ttyUSB0 write_flash 0x18000 "$BIN"
```
