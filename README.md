# Matter Raw Stream

A **raw byte-streaming device** for Matter. Bulk payloads arrive as chunked writes on a
custom vendor cluster (`0x1618FC01`), are verified end to end, persisted to flash, and
replayed on demand. The reference application is a **48-LED (8x6) WS2812 matrix animation
light** on ESP32, where the stream is a sequence of RGB frames; interactive control (on/off,
brightness, color) comes from the standard Matter light clusters.

Stream transport properties:

- Chunked writes with a self-describing header (`frame_index`, `count`, payload dims, fps)
- SHA-256 verification of the complete payload before it becomes playable
- Flash persistence in 5 LRU slots, so a payload replays without re-streaming
- Dedicated io task keeps all flash IO off the Matter stack
- Robust against retransmitted, duplicated, or out-of-order chunks
- Status, active-payload hash, and cached-payload list are exposed as attributes

- **Vendor / Product:** Matterize Labs `0x1618` / `0x0001` (factory DAC)
- **Target:** ESP32 (classic, no PSRAM), flashed via `/dev/ttyUSB0`
- **Toolchain:** esp-idf `v6.0.2`, esp-matter `release-v1.6`

## How it works

```
raw payload (e.g. RGB frames)
   └─ tools/effects.py   generate → frames → SHA-256 + hex chunks
        └─ chip-tool write-by-id (custom cluster 0x1618FC01)
             └─ ESP32 io task: dedup, flash cache (5 slots, LRU), hash verify
                  └─ playback task: RAM jitter buffer → RMT → WS2812
```

Transfer protocol (all writable attributes are octet strings, written as `hex:`):

1. `TransferHash`  (`0x0005`) - announce target payload (SHA-256)
2. `TransferMeta`  (`0x0006`) - `[total_frames u16][fps u8][loop u8][w u8][h u8]`
3. `FrameChunk`    (`0x0007`) - `[frame_index u16][count u8][w u8][h u8][fps u8][RGB…]`
4. `PlayCmd`       (`0x0009`) - `hex:01` PLAY, `hex:02` STOP, `hex:03` CLEAR_CACHE

Resending only `TransferHash` + `PlayCmd` for a cached hash replays from flash. A payload
is only committed to the cache after its flash contents verify against the announced hash.

## Example usage

`ct` below is the chip-tool alias, device is node `1`, endpoint `1`.

### 1. Build and flash

```bash
export IDF_PATH=$HOME/.espressif/versions/esp-idf/v6.0.2
export ESP_MATTER_PATH=$HOME/.espressif/versions/esp-matter/release-v1.6
. $IDF_PATH/export.sh
. $ESP_MATTER_PATH/export.sh

idf.py build
idf.py -p /dev/ttyUSB0 flash
```

Flash the factory partition (DAC/PAI/CD, not written by `idf.py flash`) once:

```bash
BIN=tools/certs/out/1618_1/<uuid>/<uuid>-partition.bin
esptool.py --port /dev/ttyUSB0 write_flash 0x18000 "$BIN"
```

### 2. Commission

```bash
chip-tool pairing code 1 MT:UFEA08-E150QJ850Y10      # or: pairing onnetwork 1 89674523 2245
```

### 3. Stream a payload

Generate a chase effect as paste-ready commands:

```bash
cd tools
uv run python effects.py chase --leds 2 --seconds 0.1 --color ff8800 --node-id 1 > /tmp/chase.sh
```

The script announces the payload, sends metadata and one chunk, then plays:

```text
# chase: leds=0..1 hold=3frames(0.1s) frames=6 chunks=1 hash=9521b8e42a9c6f7f988c17618a3a85d997c6c3ded58188a1b68af5eb2502429b
./chip-tool any write-by-id 0x1618FC01 0x0005 hex:9521b8e42a9c6f7f988c17618a3a85d997c6c3ded58188a1b68af5eb2502429b 1 1
./chip-tool any write-by-id 0x1618FC01 0x0006 hex:06001e010806 1 1
./chip-tool any write-by-id 0x1618FC01 0x0007 hex:00000608061eff8800000000000000... 1 1   # 870-byte chunk, elided
./chip-tool any write-by-id 0x1618FC01 0x0009 hex:01 1 1
```

Run it and check the transfer status (4 = playing):

```bash
sh /tmp/chase.sh
ct any read-by-id 0x1618FC01 0x0008 1 1
```

### 4. Control the light live (standard clusters)

```bash
ct onoff on 1 1
ct levelcontrol move-to-level 128 0 0 0 1 1               # brightness 50%
ct colorcontrol move-to-hue-and-saturation 200 100 0 0 0 1 1   # hue 200 deg, sat 100%
ct colorcontrol move-to-color-temperature 250 0 0 0 1 1   # 250 mireds (approx 4000K)
```

### 5. Replay from the flash cache

Only the hash and the play command are needed; nothing is re-streamed:

```bash
ct any write-by-id 0x1618FC01 0x0005 hex:9521b8e42a9c6f7f988c17618a3a85d997c6c3ded58188a1b68af5eb2502429b 1 1
ct any write-by-id 0x1618FC01 0x0009 hex:01 1 1
```

### 6. Stop and clear

```bash
ct any write-by-id 0x1618FC01 0x0009 hex:02 1 1   # STOP, matrix cleared
ct any write-by-id 0x1618FC01 0x0009 hex:03 1 1   # CLEAR_CACHE, drop all cached payloads
```

## Repository layout

```
CMakeLists.txt            ESP-IDF project (board path set here, no env var needed)
sdkconfig.defaults        project defaults
partitions.csv            app + factory (fctry) + anim_cache
main/                     app glue: app_main + app_driver + Kconfig.projbuild
components/
├── ws2812_matrix/        board HAL + LED driver + button + color math
└── animation/            flash cache + streaming engine + wire codec + custom cluster
shared/
└── wire_contract.json    single source of truth for the wire protocol
tools/
├── generate_wire.py      wire_contract.json → anim_protocol.h + protocol.py
├── effects.py            reference payload generator (LED effects) → chip-tool commands
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

## Hardware config

Config lives in `main/Kconfig.projbuild` (defaults):

| Option | Default |
|---|---|
| `CONFIG_WS2812_GPIO` | `3` |
| `CONFIG_MATRIX_WIDTH` / `_HEIGHT` | `8` / `6` |
| `CONFIG_MATRIX_SERPENTINE` | `y` |
| `CONFIG_ANIM_FPS` | `30` |

## Payload generator

The reference light payload is a sequence of RGB frames. Generate effects as paste-ready
`chip-tool any write-by-id` commands (hash → meta → chunks → play). Run each as
`uv run python effects.py ...`:

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

## chip-tool reference

Commands below assume `ct` is the chip-tool alias, the device is **node `1`, endpoint `1`**.

Animation cluster (`0x1618FC01`, `any` commands):

```bash
ct any read-by-id 0x1618FC01 0x0008 1 1            # read transfer status
ct any write-by-id 0x1618FC01 0x0009 hex:01 1 1    # PLAY
ct any write-by-id 0x1618FC01 0x0009 hex:02 1 1    # STOP
ct any write-by-id 0x1618FC01 0x0009 hex:03 1 1    # CLEAR_CACHE
```

Transfer status (`0x0008`): `0` idle, `1` announced, `2` receiving, `4` playing, `5` error.

Standard light clusters (interactive control):

```bash
ct onoff on 1 1
ct onoff off 1 1
ct levelcontrol move-to-level 128 0 0 0 1 1                              # brightness 50%
ct colorcontrol move-to-hue-and-saturation 200 100 0 0 0 1 1              # hue 200 deg, sat 100%
ct colorcontrol move-to-color-temperature 250 0 0 0 1 1                   # 250 mireds (approx 4000K)
```

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
