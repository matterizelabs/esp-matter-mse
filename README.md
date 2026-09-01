# esp-matter-mse

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)

Matter lets manufacturers add custom clusters, attributes, and commands on top of the standard
device types through **Manufacturer Specific Extensions (MSE)**. This project is a working
reference: it defines a custom cluster (`0x1618FC01`, Matterize vendor ID `0x1618`) that streams
raw byte payloads to an ESP32, verifies them with SHA-256, caches them in flash across 5 LRU
slots, and replays them on demand. The demo sink is a 48-LED (8x6) WS2812 matrix light.

## Demo

The `fire` effect (`effects.py fire`) streaming to the 48-LED matrix:

https://github.com/user-attachments/assets/2eec36fc-0880-459b-8c0d-9bfa555e519f

All commands below use `ct` as the chip-tool alias: `alias ct=./chip-tool`.

## Quick start

### 1. Build and flash

```bash
export IDF_PATH=$HOME/.espressif/versions/esp-idf/v6.0.2
export ESP_MATTER_PATH=$HOME/.espressif/versions/esp-matter/release-v1.6
. $IDF_PATH/export.sh
. $ESP_MATTER_PATH/export.sh

idf.py build
idf.py -p /dev/ttyUSB0 flash
```

Flash the factory partition (DAC/PAI/CD) once, not written by `idf.py flash`:

```bash
BIN=tools/certs/out/1618_1/<uuid>/<uuid>-partition.bin
esptool.py --port /dev/ttyUSB0 write_flash 0x18000 "$BIN"
```

### 2. Commission

```bash
ct pairing code 1 MT:UFEA08-E150QJ850Y10   # or: ct pairing onnetwork 1 89674523 2245
```

### 3. Stream and play

```bash
cd tools

# diagnostic: light one LED at a time to verify the physical chain order
uv run python chase.py --leds 2 --seconds 0.1 --color ff8800 > /tmp/chase.sh
sh /tmp/chase.sh

# any built-in effect (see "Effects" below)
uv run python effects.py chase --speed 30 --color ff8800 > /tmp/anim.sh
sh /tmp/anim.sh

ct any read-by-id 0x1618FC01 0x0008 1 1   # 4 = playing
```

`chase.py` and `effects.py` print a `hash -> meta -> chunks -> play` sequence of
chip-tool `write-by-id` commands on stdout:

```text
ct any write-by-id 0x1618FC01 0x0005 hex:9521b8e4...2429b 1 1    # announce (sha256)
ct any write-by-id 0x1618FC01 0x0006 hex:06001e010806 1 1        # 6 frames @30fps, 8x6
ct any write-by-id 0x1618FC01 0x0007 hex:00000608061eff8800... 1 1   # chunk
ct any write-by-id 0x1618FC01 0x0009 hex:01 1 1                  # PLAY
```

### 4. Control the light (standard clusters)

```bash
ct onoff on 1 1
ct levelcontrol move-to-level 128 0 0 0 1 1                 # brightness 50%
ct colorcontrol move-to-hue-and-saturation 200 100 0 0 0 1 1 # hue 200, sat 100
```

### 5. Replay from cache, stop, clear

```bash
ct any write-by-id 0x1618FC01 0x0005 hex:9521b8e4...2429b 1 1  # replay cached: hash only
ct any write-by-id 0x1618FC01 0x0009 hex:01 1 1
ct any write-by-id 0x1618FC01 0x0009 hex:02 1 1                 # STOP
ct any write-by-id 0x1618FC01 0x0009 hex:03 1 1                 # CLEAR_CACHE
```

## Effects

Two tools render animation frames and print a chip-tool `write-by-id` sequence
(`hash -> meta -> chunks -> play`) to stdout. Pipe to a shell, or save and run later:

```bash
cd tools
uv run python effects.py <effect> [options] | sh   # built-in effects
uv run python chase.py [options] | sh              # LED-chain diagnostic
```

- `effects.py` — nine procedural effects: `solid`, `chase`, `comet`, `pulse`,
  `rainbow`, `sparkle`, `wipe`, `strobe`, `fire`.
- `chase.py` — lights one LED at a time to verify the physical chain order.

### Prefix and node id

The emitted commands call chip-tool via `./chip-tool` by default. Point
`--prefix` at your chip-tool if it lives elsewhere, and `--node-id` at the
commissioned node (default `1`):

```bash
uv run python effects.py solid --color ff8800 --prefix ~/chip-tool/chip-tool | sh
uv run python chase.py --prefix ct --node-id 1 | sh
```

`--prefix ct` emits `ct ...` commands. Note `alias ct=./chip-tool` is a shell
alias, so it works for interactive one-liners like `ct onoff on 1 1` but NOT
inside `sh /tmp/script.sh`. For scripts use `./chip-tool`, a full path, or a
real `ct` symlink/wrapper on `PATH`.

### effects.py

| Effect | Example |
|---|---|
| `solid` | `effects.py solid --color ff8800 --seconds 2` |
| `chase` | `effects.py chase --color ff0000 --speed 30 --tail 4` |
| `comet` | `effects.py comet --color 00aaff --speed 15 --tail 8` |
| `pulse` | `effects.py pulse --color ff8800 --period 2` |
| `rainbow` | `effects.py rainbow --seconds 6` |
| `sparkle` | `effects.py sparkle --color ffffff --density 0.3` |
| `wipe` | `effects.py wipe --color 00aaff --direction ltr` |
| `strobe` | `effects.py strobe --color ffffff --rate 4` |
| `fire` | `effects.py fire --seed 3` |

Common options: `--color RRGGBB` (`#` optional), `--seconds`, `--fps 30`,
`--node-id 1`, `--prefix ./chip-tool`, plus per-effect options (`--speed`,
`--size`, `--tail`, `--direction`, `--period`, `--density`, `--rate`, `--seed`).

### chase.py

```bash
uv run python chase.py --leds 30 --seconds 0.1 --color ff8800 | sh
```

`--leds` = LEDs to light in sequence (default 30, capped at `width*height`);
`--seconds` = hold time per LED. Use it to confirm the serpentine chain order
in `shared/wire_contract.json` matches the physical wiring.

## Cluster (0x1618FC01)

| Attribute | ID | Layout |
|---|---|---|
| MatrixWidth / Height | `0x0000`/`0x0001` | u16 |
| PixelCount | `0x0002` | u16 |
| Serpentine | `0x0003` | bool |
| Cached | `0x0004` | 5 x 32B slot hashes |
| TransferHash | `0x0005` | sha256, announce payload |
| TransferMeta | `0x0006` | `total_frames u16, fps u8, loop u8, w u8, h u8` |
| FrameChunk | `0x0007` | `frame_index u16, count u8, w u8, h u8, fps u8, RGB...` |
| Status | `0x0008` | `0` idle `1` announced `2` receiving `4` playing `5` error |
| PlayCmd | `0x0009` | `1` play `2` stop `3` clear cache |
| Active | `0x000A` | 32B sha256 of playing payload |

## Layout

```
main/               app_main, app_driver, Kconfig.projbuild
components/
  ws2812_matrix/    LED driver, button, color math
  stream_engine/    stream engine, flash cache, wire codec, cluster
shared/wire_contract.json   protocol source of truth
tools/              effects.py, codec, tests, certs
docs/esp-matter-patches/    required SDK patch
```

## Prereqs and config

esp-idf `v6.0.2`, esp-matter `release-v1.6` (with long-octet-string patch,
  `docs/esp-matter-patches/README.md`), Python 3.14+ with `uv`

| Kconfig | Default |
|---|---|
| `CONFIG_WS2812_GPIO` | `3` |
| `CONFIG_MATRIX_WIDTH` / `_HEIGHT` | `8` / `6` |
| `CONFIG_MATRIX_SERPENTINE` | `y` |
| `CONFIG_STREAM_FPS` | `30` |

Regenerate wire artifacts after editing `wire_contract.json`:

```bash
python3 tools/generate_wire.py
```

Pairing (dev material): setup code `2048-915-4736`, QR `MT:UFEA08-E150QJ850Y10`.

## License

Apache-2.0. See [LICENSE](LICENSE). Built by [Matterize Labs](https://matterizelabs.com).
