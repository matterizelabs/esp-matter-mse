# Matter WS2812 Animation Light — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Matter extended-color-light on ESP32 that plays looping Lottie-sourced animations on a 48-LED (8×6) WS2812 matrix, driven by a local Python CLI that emits paste-ready chip-tool commands.

**Architecture:** A companion Python tool renders Lottie → 48-LED RGB frames → wire payloads (SHA-256 + hex chunks). The ESP32 exposes a custom Matter vendor cluster (`0x1618FC01`, attribute-based so chip-tool `write-by-id` works with no rebuild). Frames stream in, are played from a RAM jitter buffer at 30 fps, and are persisted to a flash cache (5 slots, LRU) by a background task. Standard OnOff/Level/ColorControl clusters layer on top (power / brightness-multiplier / exit-to-static).

**Tech Stack:** esp-idf `v6.0.2`, esp-matter `release-v1.6`, ESP32 (RMT + `led_strip`), C++17 firmware; Python 3.14 + `rlottie-python` 1.3.8 + Pillow + stdlib `zipfile`/`hashlib`/`struct`; `pytest` for tool tests; `chip-tool` (interactive) for the controller.

**Spec:** `docs/superpowers/specs/2026-08-25-matter-ws2812-animation-design.md`

## Global Constraints

- VID `0x1618`, PID `0x0001`; custom cluster ID `0x1618FC01` (= `VID<<16 | 0xFC01`).
- 48 LEDs = 8 wide × 6 high; frame = 144 B; 30 fps; max 900 frames (30 s) per animation.
- Wire format (little-endian): `FrameChunk = [frame_index u16][count u8][width u8][height u8][fps u8][RGB…]`; `TransferMeta = [total_frames u16][fps u8][loop u8][width u8][height u8]`; hash = SHA-256 over concatenated frames (144 B each).
- All writable cluster attributes are octet strings; chip-tool writes use `hex:` encoding. Control via `chip-tool any write-by-id <cluster> <attr> <val> <node-id> <endpoint-id>`.
- Target `esp32`, flashed at `/dev/ttyUSB0`. esp-idf `v6.0.2`, esp-matter `release-v1.6`.
- Playback reads ONLY from RAM; network/flash never gate a frame.

---

## File Structure

**Python tool (`tools/`):**
- `tools/lottie2matter.py` — CLI entry point
- `tools/matter_anim/__init__.py`
- `tools/matter_anim/loader.py` — input detection + `.lottie`/`.json` extraction
- `tools/matter_anim/render.py` — rlottie-python rendering → frames
- `tools/matter_anim/codec.py` — serpentine map, frame/chunk encode, SHA-256, meta
- `tools/matter_anim/cli.py` — argparse + chip-tool command emission
- `tools/tests/test_loader.py`, `test_codec.py`, `test_render.py`, `test_cli.py`

**Firmware (`firmware/`):**
- `firmware/CMakeLists.txt`, `firmware/sdkconfig.defaults`, `firmware/main/idf_component.yml`
- `firmware/main/CMakeLists.txt`, `firmware/main/Kconfig.projbuild`, `firmware/main/partitions.csv`
- `firmware/main/app_main.cpp`, `app_driver.cpp`, `app_priv.h` (from light example, adapted)
- `firmware/components/ws2812_board/device.c`, `esp_matter_device.cmake` (board profile, `led_type=ws2812`)
- `firmware/main/ws2812_matrix.c/.h` — LED strip driver
- `firmware/main/anim_codec.c/.h` — wire header parse + serpentine (shared logic)
- `firmware/main/anim_cluster.cpp/.h` — custom cluster data model
- `firmware/main/anim_engine.cpp/.h` — jitter buffer + playback task + transfer state machine
- `firmware/main/anim_flash.cpp/.h` — flash cache (slots, LRU, hash verify)

---

## Phase A — Python tool (pure local, TDD)

### Task 1: Tool scaffold + input loader

**Files:**
- Create: `tools/matter_anim/__init__.py`, `tools/matter_anim/loader.py`, `tools/lottie2matter.py`, `tools/tests/test_loader.py`
- Create: `tools/requirements.txt`, `tools/tests/conftest.py`

**Interfaces:**
- Produces: `loader.load_animation(path: str) -> dict` (returns the Lottie JSON dict for any accepted input); `loader.SUPPORTED = ('.lottie', '.json')`.

- [ ] **Step 1: Write the failing tests**

```python
# tools/tests/test_loader.py
import json, zipfile, os
from matter_anim import loader

def _write_lottie_json(tmp_path, data):
    p = tmp_path / "a.json"
    p.write_text(json.dumps(data))
    return str(p)

def _write_dotlottie(tmp_path, lottie_dict):
    p = tmp_path / "a.lottie"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("manifest.json", json.dumps({"animations": [{"id": "anim1"}]}))
        z.writestr("animations/anim1.json", json.dumps(lottie_dict))
    return str(p)

def test_loads_plain_json(tmp_path):
    d = {"v": "5.7.4", "fr": 30, "ip": 0, "op": 60, "layers": []}
    out = loader.load_animation(_write_lottie_json(tmp_path, d))
    assert out["v"] == "5.7.4"

def test_loads_dotlottie(tmp_path):
    d = {"v": "5.7.4", "fr": 30, "ip": 0, "op": 60, "layers": []}
    out = loader.load_animation(_write_dotlottie(tmp_path, d))
    assert out["v"] == "5.7.4"

def test_rejects_unknown_extension(tmp_path):
    p = tmp_path / "a.txt"
    p.write_text("x")
    import pytest
    with pytest.raises(ValueError):
        loader.load_animation(str(p))
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd tools && python -m pytest tests/test_loader.py -v`
Expected: FAIL (module not found).

- [ ] **Step 3: Implement**

```python
# tools/matter_anim/loader.py
import json, os, zipfile

SUPPORTED = (".lottie", ".json")

def _extract_dotlottie(path: str) -> dict:
    with zipfile.ZipFile(path) as z:
        manifest = json.loads(z.read("manifest.json"))
        anim = manifest["animations"][0]
        anim_id = anim.get("id") if isinstance(anim, dict) else anim
        name = f"animations/{anim_id}.json"
        if name not in z.namelist():
            # fall back to the first animations/*.json
            name = next(n for n in z.namelist() if n.startswith("animations/") and n.endswith(".json"))
        return json.loads(z.read(name))

def load_animation(path: str) -> dict:
    ext = os.path.splitext(path)[1].lower()
    if ext == ".lottie":
        return _extract_dotlottie(path)
    if ext == ".json":
        with open(path) as f:
            return json.load(f)
    raise ValueError(f"unsupported input: {path!r}; expected .lottie or .json")
```

```python
# tools/lottie2matter.py (stub for now)
def main():
    raise SystemExit("not yet implemented")
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd tools && python -m pytest tests/test_loader.py -v`
Expected: PASS (3 passed).

- [ ] **Step 5: Commit**

```bash
git add tools/ && git commit -m "feat(tool): input loader for .lottie and .json"
```

### Task 2: Wire codec — serpentine map, frame/chunk encode, SHA-256, meta

**Files:**
- Create: `tools/matter_anim/codec.py`, `tools/tests/test_codec.py`

**Interfaces:**
- Produces: `serpentine_chain_order(width, height, serpentine) -> list[(x,y)]`; `frame_to_bytes(frame_pixels, order) -> bytes`; `encode_chunk(frame_index, frames, width, height, fps) -> bytes`; `encode_meta(total_frames, fps, loop, width, height) -> bytes`; `animation_hash(frames) -> bytes`; `pack_chunks(frames, width, height, fps) -> list[bytes]`; `CLUSTER_ID = 0x1618FC01`.

- [ ] **Step 1: Write the failing tests**

```python
# tools/tests/test_codec.py
import hashlib, struct
from matter_anim import codec

def test_serpentine_2x3():
    # chain order for W=2,H=3 serpentine:
    # row0 L->R: (0,0),(1,0) ; row1 R->L: (1,1),(0,1) ; row2 L->R: (0,2),(1,2)
    assert codec.serpentine_chain_order(2, 3, True) == [(0,0),(1,0),(1,1),(0,1),(0,2),(1,2)]

def test_linear_2x2():
    assert codec.serpentine_chain_order(2, 2, False) == [(0,0),(1,0),(0,1),(1,1)]

def test_frame_to_bytes_maps_chain_order():
    # frame_pixels[y][x] = (r,g,b)
    frame = [[(255,0,0),(0,255,0)],[(0,0,255),(255,255,255)]]
    order = codec.serpentine_chain_order(2, 2, True)  # (0,0),(1,0),(1,1),(0,1)
    out = codec.frame_to_bytes(frame, order)
    assert out == bytes([255,0,0, 0,255,0, 255,255,255, 0,0,255])

def test_encode_chunk_header():
    frames = [bytes(12)] * 2  # 2 frames x 12 bytes
    out = codec.encode_chunk(5, frames, 2, 2, 30)
    assert out[:6] == struct.pack("<HBBBB", 5, 2, 2, 2, 30)
    assert len(out) == 6 + 24

def test_encode_meta():
    assert codec.encode_meta(900, 30, 1, 8, 6) == struct.pack("<HBBBB", 900, 30, 1, 8, 6)

def test_animation_hash_is_sha256_of_concat():
    frames = [b"\x00"*144, b"\xff"*144]
    assert codec.animation_hash(frames) == hashlib.sha256(b"\x00"*144 + b"\xff"*144).digest()

def test_pack_chunks_max_1kb():
    frames = [bytes([i % 256])*144 for i in range(900)]
    chunks = codec.pack_chunks(frames, 8, 6, 30)
    assert all(len(c) <= 1000 for c in chunks)
    # 6 frames/chunk = 6*144 + 6 header = 870 bytes
    assert len(chunks) == (900 + 5) // 6
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd tools && python -m pytest tests/test_codec.py -v`
Expected: FAIL (module not found).

- [ ] **Step 3: Implement**

```python
# tools/matter_anim/codec.py
import hashlib, struct

CLUSTER_ID = 0x1618FC01
ATTR_TRANSFER_HASH = 0x0005
ATTR_TRANSFER_META = 0x0006
ATTR_FRAME_CHUNK = 0x0007
ATTR_PLAY_CMD = 0x0009
MAX_CHUNK_BYTES = 1000

def serpentine_chain_order(width, height, serpentine=True):
    order = []
    for y in range(height):
        row = range(width) if (not serpentine or y % 2 == 0) else range(width - 1, -1, -1)
        for x in row:
            order.append((x, y))
    return order

def frame_to_bytes(frame_pixels, order):
    # frame_pixels[y][x] -> (r,g,b) tuple
    out = bytearray()
    for (x, y) in order:
        r, g, b = frame_pixels[y][x]
        out += bytes((r, g, b))
    return bytes(out)

def encode_chunk(frame_index, frames, width, height, fps):
    header = struct.pack("<HBBBB", frame_index, len(frames), width, height, fps)
    return header + b"".join(frames)

def encode_meta(total_frames, fps, loop, width, height):
    return struct.pack("<HBBBB", total_frames, fps, loop, width, height)

def animation_hash(frames):
    return hashlib.sha256(b"".join(frames)).digest()

def pack_chunks(frames, width, height, fps):
    bytes_per_frame = width * height * 3
    max_count = (MAX_CHUNK_BYTES - 6) // bytes_per_frame
    chunks = []
    for i in range(0, len(frames), max_count):
        batch = frames[i:i + max_count]
        chunks.append(encode_chunk(i, batch, width, height, fps))
    return chunks
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd tools && python -m pytest tests/test_codec.py -v`
Expected: PASS (7 passed).

- [ ] **Step 5: Commit**

```bash
git add tools/ && git commit -m "feat(tool): wire codec (serpentine, chunk, hash, meta)"
```

### Task 3: Rendering with rlottie-python

**Files:**
- Create: `tools/matter_anim/render.py`, `tools/tests/test_render.py`

**Interfaces:**
- Produces: `render_frames(lottie: dict, width, height, serpentine, max_frames=900) -> (frames: list[bytes], meta: dict)` where `meta` has keys `total_frames`, `fps`, `width`, `height`.

- [ ] **Step 1: Write the failing tests**

```python
# tools/tests/test_render.py
import json
from matter_anim import render

MINI_LOTTIE = {
    "v": "5.7.4", "fr": 30, "ip": 0, "op": 3, "w": 16, "h": 16,
    "layers": [{"ty": 4, "ind": 1, "ks": {"o": {"a": 0, "k": 100}, "r": {"a": 0, "k": 0},
        "p": {"a": 0, "k": [8, 8, 0]}, "a": {"a": 0, "k": [0, 0, 0]}, "s": {"a": 0, "k": [100, 100, 100]}},
        "shapes": [{"ty": "rc", "p": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [16, 16]},
                     "nm": "r", "d": 1}], "ip": 0, "op": 3, "st": 0, "bm": 0}]
}

def test_render_frames_shape_and_count(tmp_path):
    p = tmp_path / "m.json"
    p.write_text(json.dumps(MINI_LOTTIE))
    frames, meta = render.render_frames(json.loads(p.read_text()), 8, 6, True, max_frames=900)
    assert meta["total_frames"] == 3
    assert meta["width"] == 8 and meta["height"] == 6
    assert len(frames) == 3
    assert all(len(f) == 8 * 6 * 3 for f in frames)

def test_render_caps_at_max_frames(tmp_path):
    big = dict(MINI_LOTTIE, op=2000)
    p = tmp_path / "big.json"
    p.write_text(json.dumps(big))
    frames, meta = render.render_frames(json.loads(p.read_text()), 8, 6, True, max_frames=900)
    assert meta["total_frames"] == 900
    assert len(frames) == 900
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd tools && python -m pytest tests/test_render.py -v`
Expected: FAIL (module not found).

- [ ] **Step 3: Implement**

```python
# tools/matter_anim/render.py
import json, tempfile, os
from rlottie_python import LottieAnimation
from matter_anim import codec

def render_frames(lottie: dict, width, height, serpentine=True, max_frames=900):
    # rlottie loads from a file path
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
        f.write(json.dumps(lottie).encode())
        path = f.name
    try:
        anim = LottieAnimation.from_file(path)
        w, h = anim.lottie_animation_get_size()
        total = int(anim.lottie_animation_get_totalframe())
        total = min(total, max_frames)
        order = codec.serpentine_chain_order(width, height, serpentine)
        frames = []
        for i in range(total):
            img = anim.render_pillow_frame(i, width=w, height=h).resize((width, height))
            rgb = img.convert("RGB")
            px = rgb.load()
            grid = [[tuple(px[x, y]) for x in range(width)] for y in range(height)]
            frames.append(codec.frame_to_bytes(grid, order))
        fps = int(anim.lottie_animation_get_framerate())
        return frames, {"total_frames": total, "fps": fps, "width": width, "height": height}
    finally:
        os.unlink(path)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd tools && python -m pytest tests/test_render.py -v`
Expected: PASS (2 passed).

- [ ] **Step 5: Commit**

```bash
git add tools/ && git commit -m "feat(tool): rlottie-python rendering to 8x6 RGB frames"
```

### Task 4: CLI + paste-ready chip-tool output

**Files:**
- Create: `tools/matter_anim/cli.py`, rewrite `tools/lottie2matter.py`, `tools/tests/test_cli.py`

**Interfaces:**
- Produces: `cli.build_commands(node_id, hash_bytes, meta_bytes, chunks) -> list[str]`; `main(argv) -> int` (CLI entry).

- [ ] **Step 1: Write the failing tests**

```python
# tools/tests/test_cli.py
from matter_anim import cli

def test_build_commands_sequence():
    node = 1234
    h = bytes.fromhex("ab" * 32)
    meta = bytes(6)
    chunks = [bytes.fromhex("cd" * 870)]
    cmds = cli.build_commands(node, h, meta, chunks)
    assert cmds[0] == f"./chip-tool any write-by-id 0x1618FC01 0x0005 hex:{h.hex()} 1234 1"
    assert cmds[1] == f"./chip-tool any write-by-id 0x1618FC01 0x0006 hex:{meta.hex()} 1234 1"
    assert cmds[2] == f"./chip-tool any write-by-id 0x1618FC01 0x0007 hex:{chunks[0].hex()} 1234 1"
    assert cmds[-1] == f"./chip-tool any write-by-id 0x1618FC01 0x0009 hex:01 1234 1"

def test_main_end_to_end(tmp_path, capsys):
    import json, subprocess, sys
    lottie = {"v":"5.7.4","fr":30,"ip":0,"op":2,"w":16,"h":16,
              "layers":[{"ty":4,"ind":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},
              "p":{"a":0,"k":[8,8,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},
              "shapes":[{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[16,16]},"nm":"r","d":1}],
              "ip":0,"op":2,"st":0,"bm":0}]}
    p = tmp_path / "m.json"
    p.write_text(json.dumps(lottie))
    rc = cli.main([str(p), "--node-id", "7"])
    out = capsys.readouterr().out
    assert rc == 0
    assert "any write-by-id 0x1618FC01 0x0005 hex:" in out
    assert out.count("0x0007 hex:") >= 1
    assert "0x0009 hex:01 7 1" in out
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd tools && python -m pytest tests/test_cli.py -v`
Expected: FAIL (module not found).

- [ ] **Step 3: Implement**

```python
# tools/matter_anim/cli.py
import argparse, sys
from matter_anim import loader, render, codec

def build_commands(node_id, hash_bytes, meta_bytes, chunks):
    cid = hex(codec.CLUSTER_ID)
    cmds = []
    cmds.append(f"./chip-tool any write-by-id {cid} {hex(codec.ATTR_TRANSFER_HASH)} hex:{hash_bytes.hex()} {node_id} 1")
    cmds.append(f"./chip-tool any write-by-id {cid} {hex(codec.ATTR_TRANSFER_META)} hex:{meta_bytes.hex()} {node_id} 1")
    for ch in chunks:
        cmds.append(f"./chip-tool any write-by-id {cid} {hex(codec.ATTR_FRAME_CHUNK)} hex:{ch.hex()} {node_id} 1")
    cmds.append(f"./chip-tool any write-by-id {cid} {hex(codec.ATTR_PLAY_CMD)} hex:01 {node_id} 1")
    return cmds

def main(argv=None):
    p = argparse.ArgumentParser(prog="lottie2matter.py")
    p.add_argument("input", help=".lottie or .json file")
    p.add_argument("--node-id", type=int, default=1)
    p.add_argument("--width", type=int, default=8)
    p.add_argument("--height", type=int, default=6)
    p.add_argument("--serpentine", action="store_true", default=True)
    p.add_argument("--no-serpentine", dest="serpentine", action="store_false")
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--max-frames", type=int, default=900)
    args = p.parse_args(argv)

    lottie = loader.load_animation(args.input)
    frames, meta = render.render_frames(lottie, args.width, args.height, args.serpentine, args.max_frames)
    h = codec.animation_hash(frames)
    meta_bytes = codec.encode_meta(meta["total_frames"], args.fps, 1, args.width, args.height)
    chunks = codec.pack_chunks(frames, args.width, args.height, args.fps)

    for c in build_commands(args.node_id, h, meta_bytes, chunks):
        print(c)
    print(f"# hash={h.hex()} frames={meta['total_frames']} fps={args.fps}", file=sys.stderr)
    return 0
```

```python
# tools/lottie2matter.py
from matter_anim.cli import main
if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd tools && python -m pytest tests/test_cli.py -v`
Expected: PASS (2 passed).

- [ ] **Step 5: Run the full tool suite**

Run: `cd tools && python -m pytest -v`
Expected: PASS (14 tests).

- [ ] **Step 6: Commit**

```bash
git add tools/ && git commit -m "feat(tool): CLI emitting paste-ready chip-tool commands"
```

---

## Phase B — Firmware scaffold + matrix driver

### Task 5: Firmware scaffold from the light example

**Files:**
- Create: `firmware/CMakeLists.txt`, `firmware/sdkconfig.defaults`, `firmware/main/CMakeLists.txt`, `firmware/main/idf_component.yml`, `firmware/main/Kconfig.projbuild`, `firmware/main/partitions.csv`, `firmware/main/app_priv.h`
- Copy (then adapt in later tasks): `firmware/main/app_main.cpp`, `firmware/main/app_driver.cpp` from `~/.espressif/versions/esp-matter/release-v1.6/examples/light/main/`
- Create board profile: `firmware/components/ws2812_board/device.c`, `esp_matter_device.cmake`

**Interfaces:**
- Produces: a project that builds and flashes the stock extended color light on ESP32 with `led_type=ws2812`.

- [ ] **Step 1: Copy the light example sources**

```bash
mkdir -p firmware/main firmware/components/ws2812_board
cp ~/.espressif/versions/esp-matter/release-v1.6/examples/light/main/app_main.cpp firmware/main/
cp ~/.espressif/versions/esp-matter/release-v1.6/examples/light/main/app_driver.cpp firmware/main/
cp ~/.espressif/versions/esp-matter/release-v1.6/examples/light/main/app_priv.h firmware/main/
cp ~/.espressif/versions/esp-matter/release-v1.6/examples/light/CMakeLists.txt firmware/CMakeLists.txt
cp ~/.espressif/versions/esp-matter/release-v1.6/examples/light/main/CMakeLists.txt firmware/main/CMakeLists.txt
cp ~/.espressif/versions/esp-matter/release-v1.6/examples/light/main/idf_component.yml firmware/main/idf_component.yml
```

- [ ] **Step 2: Write the board profile**

```c
// firmware/components/ws2812_board/device.c
#include <esp_log.h>
#include <led_driver.h>
#include <button_gpio.h>

led_driver_config_t led_driver_get_config() {
    led_driver_config_t config = { .gpio = CONFIG_WS2812_GPIO, .channel = 0 };
    return config;
}
button_gpio_config_t button_driver_get_config() {
    button_gpio_config_t config = { .gpio_num = GPIO_NUM_0, .active_level = 0 };
    return config;
}
```

```cmake
# firmware/components/ws2812_board/esp_matter_device.cmake
cmake_minimum_required(VERSION 3.5)
if (NOT ("${IDF_TARGET}" STREQUAL "esp32"))
    message(FATAL_ERROR "please set esp32 as the IDF_TARGET")
endif()
SET(device_type     ws2812_board)
SET(led_type        ws2812)
SET(button_type     iot)
SET(extra_components_dirs_append
    "$ENV{ESP_MATTER_DEVICE_PATH}/../../led_driver"
    "$ENV{ESP_MATTER_DEVICE_PATH}/../../button_driver/iot_button")
```

- [ ] **Step 3: Write Kconfig, partitions, sdkconfig.defaults**

```kconfig
# firmware/main/Kconfig.projbuild
menu "WS2812 Matrix"
    config WS2812_GPIO
        int "WS2812 data GPIO"
        default 5
        range 0 39
    config MATRIX_WIDTH
        int "Matrix width"
        default 8
    config MATRIX_HEIGHT
        int "Matrix height"
        default 6
    config MATRIX_SERPENTINE
        bool "Serpentine (zigzag) wiring"
        default y
    config ANIM_FPS
        int "Animation FPS"
        default 30
endmenu
```

```csv
# firmware/main/partitions.csv
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   0x4000
otadata,    data, ota,     0xD000,   0x2000
phy_init,   data, phy,     0xF000,   0x1000
fctry,      data, nvs,     0x10000,  0x6000
factory,    app,  factory, 0x20000,  0x1C0000
anim_cache, data, fat,     0x1E0000, 0xC0000
```

```ini
# firmware/sdkconfig.defaults
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT=n
CONFIG_LWIP_IPV6_AUTOCONFIG=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0xC000
CONFIG_ENABLE_CHIP_SHELL=y
CONFIG_ENABLE_WIFI_AP=n
CONFIG_LWIP_HOOK_IP6_ROUTE_DEFAULT=y
CONFIG_LWIP_HOOK_ND6_GET_GW_DEFAULT=y
CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=2
CONFIG_BUTTON_PERIOD_TIME_MS=20
CONFIG_BUTTON_LONG_PRESS_TIME_MS=5000
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n
CONFIG_ENABLE_OTA_REQUESTOR=n
CONFIG_MBEDTLS_HKDF_C=y
CONFIG_LWIP_IPV6_NUM_ADDRESSES=6
```

- [ ] **Step 4: Build**

```bash
export IDF_PATH=~/.espressif/versions/esp-idf/v6.0.2
export ESP_MATTER_PATH=~/.espressif/versions/esp-matter/release-v1.6
export ESP_MATTER_DEVICE_PATH=$PWD/firmware/components/ws2812_board
. $IDF_PATH/export.sh
cd firmware && idf.py set-target esp32 && idf.py build
```
Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add firmware/ && git commit -m "feat(fw): scaffold from light example with ws2812 board profile + partitions"
```

### Task 6: WS2812 matrix driver

**Files:**
- Create: `firmware/main/ws2812_matrix.c`, `ws2812_matrix.h`
- Modify: `firmware/main/CMakeLists.txt` (add sources), `firmware/main/idf_component.yml` (add `led_strip` dep)

**Interfaces:**
- Produces: `ws2812_matrix_init(void) -> ws2812_matrix_handle_t`; `ws2812_matrix_show_frame(handle, const uint8_t *rgb, size_t len)` (len = LED_COUNT*3); `ws2812_matrix_fill_rgb(handle, uint8_t r, uint8_t g, uint8_t b)`; `ws2812_matrix_clear(handle)`.

- [ ] **Step 1: Add the led_strip dependency**

```yaml
# firmware/main/idf_component.yml
dependencies:
  espressif/led_strip: "^2"
  espressif/cmake_utilities:
    version: "^1"
    rules:
      - if: "idf_version >=5.0"
      - if: "target in [esp32c2]"
```

- [ ] **Step 2: Implement the driver**

```c
// firmware/main/ws2812_matrix.h
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void *ws2812_matrix_handle_t;
ws2812_matrix_handle_t ws2812_matrix_init(void);
void ws2812_matrix_show_frame(ws2812_matrix_handle_t h, const uint8_t *rgb, size_t len);
void ws2812_matrix_fill_rgb(ws2812_matrix_handle_t h, uint8_t r, uint8_t g, uint8_t b);
void ws2812_matrix_clear(ws2812_matrix_handle_t h);
```

```c
// firmware/main/ws2812_matrix.c
#include "ws2812_matrix.h"
#include "led_strip.h"
#include "esp_log.h"

#define TAG "ws2812_matrix"
#define LED_COUNT (CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT)

ws2812_matrix_handle_t ws2812_matrix_init(void) {
    led_strip_config_t cfg = { .strip_gpio_num = CONFIG_WS2812_GPIO, .max_leds = LED_COUNT };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    led_strip_handle_t strip = NULL;
    if (led_strip_new_rmt_device(&cfg, &rmt, &strip) != ESP_OK) return NULL;
    led_strip_clear(strip);
    return (ws2812_matrix_handle_t)strip;
}

void ws2812_matrix_show_frame(ws2812_matrix_handle_t h, const uint8_t *rgb, size_t len) {
    led_strip_handle_t strip = (led_strip_handle_t)h;
    for (size_t i = 0; i < LED_COUNT && (i + 1) * 3 <= len; i++) {
        led_strip_set_pixel(strip, i, rgb[i*3], rgb[i*3+1], rgb[i*3+2]);
    }
    led_strip_refresh(strip);
}

void ws2812_matrix_fill_rgb(ws2812_matrix_handle_t h, uint8_t r, uint8_t g, uint8_t b) {
    led_strip_handle_t strip = (led_strip_handle_t)h;
    for (size_t i = 0; i < LED_COUNT; i++) led_strip_set_pixel(strip, i, r, g, b);
    led_strip_refresh(strip);
}

void ws2812_matrix_clear(ws2812_matrix_handle_t h) {
    led_strip_clear((led_strip_handle_t)h);
}
```

- [ ] **Step 3: Register sources in CMake**

```cmake
# firmware/main/CMakeLists.txt — append ws2812_matrix.c to SRC_DIRS list
idf_component_register(SRC_DIRS "." PRIV_INCLUDE_DIRS "." "${ESP_MATTER_PATH}/examples/common/utils")
```

- [ ] **Step 4: Build**

Run: `cd firmware && idf.py build`
Expected: build succeeds.

- [ ] **Step 5: Flash and smoke-test (static fill)**

Add a temporary 3-line call in `app_main()` after `app_driver_light_init()`:
```c
ws2812_matrix_handle_t m = ws2812_matrix_init();
ws2812_matrix_fill_rgb(m, 255, 0, 0);  // expect all-red matrix
```
Run: `idf.py -p /dev/ttyUSB0 flash monitor`
Expected: matrix lights solid red. Remove the temporary lines afterward.

- [ ] **Step 6: Commit**

```bash
git add firmware/ && git commit -m "feat(fw): ws2812 matrix driver (led_strip/RMT)"
```

---

## Phase C — Custom cluster

### Task 7: Custom cluster data model

**Files:**
- Create: `firmware/main/anim_cluster.cpp`, `anim_cluster.h`
- Modify: `firmware/main/app_main.cpp` (create cluster on the light endpoint)

**Interfaces:**
- Produces: `anim_cluster_create(endpoint_t *ep) -> cluster_t*`; constant `ANIM_CLUSTER_ID = 0x1618FC01`; attribute-id constants `ANIM_ATTR_*`.

- [ ] **Step 1: Implement the cluster**

```cpp
// firmware/main/anim_cluster.h
#pragma once
#include <esp_matter.h>
namespace anim {
constexpr uint32_t CLUSTER_ID = 0x1618FC01;
constexpr uint32_t ATTR_MATRIX_WIDTH = 0x0000, ATTR_MATRIX_HEIGHT = 0x0001, ATTR_PIXEL_COUNT = 0x0002, ATTR_SERPENTINE = 0x0003;
constexpr uint32_t ATTR_CACHED = 0x0004, ATTR_TRANSFER_HASH = 0x0005, ATTR_TRANSFER_META = 0x0006, ATTR_FRAME_CHUNK = 0x0007;
constexpr uint32_t ATTR_STATUS = 0x0008, ATTR_PLAY_CMD = 0x0009, ATTR_ACTIVE = 0x000A;
esp_matter::cluster_t *anim_cluster_create(esp_matter::endpoint_t *ep);
}
```

```cpp
// firmware/main/anim_cluster.cpp
#include "anim_cluster.h"
using namespace esp_matter;
static uint8_t s_hash[32] = {0}, s_meta[6] = {0}, s_frame[1100] = {0};
static uint8_t s_cmd[1] = {0}, s_active[32] = {0};

cluster_t *anim::anim_cluster_create(endpoint_t *ep) {
    cluster_t *c = cluster::create(ep, CLUSTER_ID, CLUSTER_FLAG_SERVER);
    attribute::create(c, ATTR_MATRIX_WIDTH, ATTRIBUTE_FLAG_NONE, esp_matter_uint16(CONFIG_MATRIX_WIDTH));
    attribute::create(c, ATTR_MATRIX_HEIGHT, ATTRIBUTE_FLAG_NONE, esp_matter_uint16(CONFIG_MATRIX_HEIGHT));
    attribute::create(c, ATTR_PIXEL_COUNT, ATTRIBUTE_FLAG_NONE, esp_matter_uint16(CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT));
    attribute::create(c, ATTR_SERPENTINE, ATTRIBUTE_FLAG_NONE, esp_matter_bool(CONFIG_MATRIX_SERPENTINE));
    attribute::create(c, ATTR_CACHED, ATTRIBUTE_FLAG_NONE, esp_matter_octet_str(s_active, 32), 32);
    attribute::create(c, ATTR_TRANSFER_HASH, ATTRIBUTE_FLAG_WRITABLE, esp_matter_octet_str(s_hash, 32), 32);
    attribute::create(c, ATTR_TRANSFER_META, ATTRIBUTE_FLAG_WRITABLE, esp_matter_octet_str(s_meta, 6), 6);
    attribute::create(c, ATTR_FRAME_CHUNK, ATTRIBUTE_FLAG_WRITABLE, esp_matter_octet_str(s_frame, 1100), 1100);
    attribute::create(c, ATTR_STATUS, ATTRIBUTE_FLAG_NONE, esp_matter_enum8(0));
    attribute::create(c, ATTR_PLAY_CMD, ATTRIBUTE_FLAG_WRITABLE, esp_matter_octet_str(s_cmd, 1), 1);
    attribute::create(c, ATTR_ACTIVE, ATTRIBUTE_FLAG_NONE, esp_matter_octet_str(s_active, 32), 32);
    return c;
}
```

- [ ] **Step 2: Wire the cluster into app_main**

In `firmware/main/app_main.cpp`, after the extended_color_light endpoint is created:
```cpp
endpoint_t *endpoint = extended_color_light::create(node, &light_config, ENDPOINT_FLAG_NONE, light_handle);
anim::anim_cluster_create(endpoint);
```
Add `#include "anim_cluster.h"`.

- [ ] **Step 3: Build**

Run: `cd firmware && idf.py build`
Expected: build succeeds (custom cluster compiles and registers).

- [ ] **Step 4: Flash and read the geometry attribute**

Run: `idf.py -p /dev/ttyUSB0 flash monitor`
Commission (chip-tool pairing), then:
```bash
./chip-tool any read-by-id 0x1618FC01 0x0000 <node-id> 1
```
Expected: reads back `8` (matrix width) — proves the custom cluster + attributes are live.

- [ ] **Step 5: Commit**

```bash
git add firmware/ && git commit -m "feat(fw): custom vendor cluster 0x1618FC01"
```

---

## Phase D — Animation engine

### Task 8: On-device codec (header parse + serpentine)

**Files:**
- Create: `firmware/main/anim_codec.c`, `anim_codec.h`
- Modify: `firmware/main/CMakeLists.txt`

**Interfaces:**
- Produces: `anim_parse_chunk_header(const uint8_t *buf, size_t len, anim_chunk_hdr_t *out) -> bool`; `anim_grid_to_chain(const uint8_t *grid_rgb, int width, int height, bool serpentine, uint8_t *chain_out) -> void`; structs `anim_chunk_hdr_t { uint16_t frame_index; uint8_t count, width, height, fps; }`.

- [ ] **Step 1: Implement**

```c
// firmware/main/anim_codec.h
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct { uint16_t frame_index; uint8_t count, width, height, fps; } anim_chunk_hdr_t;
bool anim_parse_chunk_header(const uint8_t *buf, size_t len, anim_chunk_hdr_t *out);
void anim_grid_to_chain(const uint8_t *grid_rgb, int width, int height, bool serpentine, uint8_t *chain_out);
```

```c
// firmware/main/anim_codec.c
#include "anim_codec.h"
bool anim_parse_chunk_header(const uint8_t *buf, size_t len, anim_chunk_hdr_t *out) {
    if (!buf || !out || len < 6) return false;
    out->frame_index = (uint16_t)(buf[0] | (buf[1] << 8));
    out->count = buf[2]; out->width = buf[3]; out->height = buf[4]; out->fps = buf[5];
    return true;
}
void anim_grid_to_chain(const uint8_t *grid, int w, int h, bool serpentine, uint8_t *chain) {
    int i = 0;
    for (int y = 0; y < h; y++) {
        for (int k = 0; k < w; k++) {
            int x = (serpentine && (y & 1)) ? (w - 1 - k) : k;
            int src = (y * w + x) * 3, dst = i * 3;
            chain[dst] = grid[src]; chain[dst+1] = grid[src+1]; chain[dst+2] = grid[src+2];
            i++;
        }
    }
}
```

- [ ] **Step 2: Build**

Run: `cd firmware && idf.py build`
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add firmware/ && git commit -m "feat(fw): on-device wire codec (header parse + serpentine)"
```

### Task 9: Jitter buffer + playback task

**Files:**
- Create: `firmware/main/anim_engine.cpp`, `anim_engine.h`

**Interfaces:**
- Produces: `anim_engine_init(ws2812_matrix_handle_t m) -> esp_err_t`; `anim_engine_push_frame(const uint8_t *chain_rgb, size_t len) -> bool`; `anim_engine_set_brightness(uint8_t pct)`; `anim_engine_play_from_cache(...) -> void` (filled in Task 11); `anim_engine_stop(void)`.

- [ ] **Step 1: Implement ring buffer + 30 fps playback task**

```cpp
// firmware/main/anim_engine.h
#pragma once
#include <esp_err.h>
#include "ws2812_matrix.h"
esp_err_t anim_engine_init(ws2812_matrix_handle_t m);
bool anim_engine_push_frame(const uint8_t *chain_rgb, size_t len);
void anim_engine_set_brightness(uint8_t pct);
void anim_engine_stop(void);
```

```cpp
// firmware/main/anim_engine.cpp
#include "anim_engine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define TAG "anim_engine"
#define LED_COUNT (CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT)
#define FRAME_BYTES (LED_COUNT * 3)
#define RING_LEN 8
#define TICK_MS (1000 / CONFIG_ANIM_FPS)

static ws2812_matrix_handle_t s_matrix;
static QueueHandle_t s_q;          // queue of pointers into a static ring
static uint8_t s_ring[RING_LEN][FRAME_BYTES];
static int s_head = 0;             // next write slot
static volatile uint8_t s_brightness = 100;
static volatile bool s_running = false;

static void playback_task(void *arg) {
    while (true) {
        if (s_running) {
            void *slot = NULL;
            if (xQueueReceive(s_q, &slot, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
                uint8_t *frame = (uint8_t *)slot;
                // apply master brightness in-place (copy to scratch)
                static uint8_t scratch[FRAME_BYTES];
                for (int i = 0; i < FRAME_BYTES; i++) scratch[i] = frame[i] * s_brightness / 100;
                ws2812_matrix_show_frame(s_matrix, scratch, FRAME_BYTES);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        }
    }
}

esp_err_t anim_engine_init(ws2812_matrix_handle_t m) {
    s_matrix = m;
    s_q = xQueueCreate(RING_LEN, sizeof(void *));
    xTaskCreate(playback_task, "anim_play", 4096, NULL, 10, NULL);
    return ESP_OK;
}

bool anim_engine_push_frame(const uint8_t *chain_rgb, size_t len) {
    if (len != FRAME_BYTES) return false;
    uint8_t *slot = s_ring[s_head % RING_LEN];
    memcpy(slot, chain_rgb, FRAME_BYTES);
    if (xQueueSend(s_q, &slot, 0) != pdTRUE) return false;  // drop if full (overrun)
    s_head++;
    return true;
}
void anim_engine_set_brightness(uint8_t pct) { s_brightness = pct > 100 ? 100 : pct; }
void anim_engine_stop(void) { s_running = false; }
```

- [ ] **Step 2: Build**

Run: `cd firmware && idf.py build`
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add firmware/ && git commit -m "feat(fw): jitter ring buffer + 30fps playback task"
```

### Task 10: Flash cache (slots, LRU, hash verify, write task)

**Files:**
- Create: `firmware/main/anim_flash.cpp`, `anim_flash.h`

**Interfaces:**
- Produces: `anim_flash_init(void) -> esp_err_t`; `anim_flash_begin(hash[32]) -> slot_id`; `anim_flash_write(slot_id, data, len) -> esp_err_t`; `anim_flash_commit(slot_id, hash[32], total_frames, fps, loop) -> esp_err_t`; `anim_flash_find(hash[32]) -> int (-1 miss)`; `anim_flash_read_frame(slot_id, frame_index, out[FRAME_BYTES]) -> esp_err_t`.

- [ ] **Step 1: Implement raw-partition fixed-slot cache**

```cpp
// firmware/main/anim_flash.h
#pragma once
#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>
#define ANIM_SLOT_COUNT 5
#define ANIM_SLOT_SIZE (128 * 1024)
#define ANIM_MAX_FRAMES 900
esp_err_t anim_flash_init(void);
int  anim_flash_find(const uint8_t hash[32]);
int  anim_flash_alloc_slot(void);
esp_err_t anim_flash_erase(int slot);
esp_err_t anim_flash_write(int slot, const uint8_t *data, size_t len);
esp_err_t anim_flash_commit(int slot, const uint8_t hash[32], uint16_t total_frames, uint8_t fps, uint8_t loop);
esp_err_t anim_flash_read_frame(int slot, uint32_t frame_index, uint8_t *out, size_t out_len);
```

```cpp
// firmware/main/anim_flash.cpp
#include "anim_flash.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>

#define TAG "anim_flash"
#define FRAME_BYTES (CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3)
#define META_SIZE 4096
#define DATA_OFFSET META_SIZE

typedef struct {
    uint32_t magic;
    uint32_t version;
    struct { uint8_t hash[32]; uint16_t total_frames; uint8_t fps, loop, state; uint32_t lru; } slots[ANIM_SLOT_COUNT];
} anim_meta_t;

static const esp_partition_t *s_part;
static anim_meta_t s_meta;
static uint32_t s_tick = 0;

esp_err_t anim_flash_init(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "anim_cache");
    if (!s_part) { ESP_LOGE(TAG, "anim_cache partition not found"); return ESP_ERR_NOT_FOUND; }
    if (esp_partition_read(s_part, 0, &s_meta, sizeof(s_meta)) != ESP_OK || s_meta.magic != 0x414E494D) {
        memset(&s_meta, 0, sizeof(s_meta)); s_meta.magic = 0x414E494D; s_meta.version = 1;
        esp_partition_erase_range(s_part, 0, s_part->size);
        esp_partition_write(s_part, 0, &s_meta, sizeof(s_meta));
    }
    return ESP_OK;
}
int anim_flash_find(const uint8_t hash[32]) {
    for (int i = 0; i < ANIM_SLOT_COUNT; i++)
        if (s_meta.slots[i].state == 1 && memcmp(s_meta.slots[i].hash, hash, 32) == 0) return i;
    return -1;
}
static int pick_lru_slot(void) {
    uint32_t min = UINT32_MAX; int pick = 0;
    for (int i = 0; i < ANIM_SLOT_COUNT; i++) if (s_meta.slots[i].lru < min) { min = s_meta.slots[i].lru; pick = i; }
    return pick;
}
int anim_flash_alloc_slot(void) { return pick_lru_slot(); }
esp_err_t anim_flash_write(int slot, const uint8_t *data, size_t len) {
    uint32_t off = DATA_OFFSET + (uint32_t)slot * ANIM_SLOT_SIZE;
    return esp_partition_write(s_part, off, data, len);
}
esp_err_t anim_flash_erase(int slot) {
    uint32_t off = DATA_OFFSET + (uint32_t)slot * ANIM_SLOT_SIZE;
    return esp_partition_erase_range(s_part, off, ANIM_SLOT_SIZE);
}
esp_err_t anim_flash_commit(int slot, const uint8_t hash[32], uint16_t total_frames, uint8_t fps, uint8_t loop) {
    memcpy(s_meta.slots[slot].hash, hash, 32);
    s_meta.slots[slot].total_frames = total_frames; s_meta.slots[slot].fps = fps;
    s_meta.slots[slot].loop = loop; s_meta.slots[slot].state = 1; s_meta.slots[slot].lru = ++s_tick;
    esp_partition_erase_range(s_part, 0, META_SIZE);
    return esp_partition_write(s_part, 0, &s_meta, sizeof(s_meta));
}
esp_err_t anim_flash_read_frame(int slot, uint32_t frame_index, uint8_t *out, size_t out_len) {
    uint32_t off = DATA_OFFSET + (uint32_t)slot * ANIM_SLOT_SIZE + frame_index * FRAME_BYTES;
    if (out_len < FRAME_BYTES) return ESP_ERR_INVALID_SIZE;
    return esp_partition_read(s_part, off, out, FRAME_BYTES);
}
```

- [ ] **Step 2: Build**

Run: `cd firmware && idf.py build`
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add firmware/ && git commit -m "feat(fw): flash cache (5 slots, LRU, hash)"
```

### Task 11: Transfer state machine + wire into the attribute callback

**Files:**
- Modify: `firmware/main/app_main.cpp` (extend `app_attribute_update_cb`), `firmware/main/anim_engine.cpp/.h`, `firmware/main/anim_flash.cpp/.h`

**Interfaces:**
- Consumes: `anim_parse_chunk_header`, `anim_engine_push_frame`, `anim_flash_*`.
- Produces: `anim_on_attr_update(attribute_id, val, val_size)` handling `TransferHash`/`TransferMeta`/`FrameChunk`/`PlayCmd`.

- [ ] **Step 1: Add the transfer state machine to anim_engine**

```cpp
// firmware/main/anim_engine.h — add
void anim_handle_frame_chunk(const uint8_t *data, size_t len);
void anim_handle_transfer_hash(const uint8_t *hash, size_t len);
void anim_handle_transfer_meta(const uint8_t *meta, size_t len);
void anim_handle_play_cmd(uint8_t cmd);
void anim_set_status(uint8_t status);
```

```cpp
// firmware/main/anim_engine.cpp — add
#include "anim_codec.h"
#include "anim_flash.h"
#include "mbedtls/sha256.h"

static int s_slot = -1; static uint32_t s_write_cursor = 0; static uint16_t s_total = 0;
static uint8_t s_pending_hash[32]; static mbedtls_sha256_context s_hash_ctx;

void anim_handle_transfer_hash(const uint8_t *hash, size_t len) {
    if (len != 32) return;
    memcpy(s_pending_hash, hash, 32);
    s_slot = anim_flash_find(hash);
    anim_set_status(s_slot >= 0 ? 4 /*READY*/ : 1 /*ANNOUNCED*/);
    if (s_slot < 0) { s_slot = anim_flash_alloc_slot(); anim_flash_erase(s_slot); s_write_cursor = 0; }
    mbedtls_sha256_init(&s_hash_ctx); mbedtls_sha256_starts(&s_hash_ctx, 0);
}
void anim_handle_frame_chunk(const uint8_t *data, size_t len) {
    anim_chunk_hdr_t hdr;
    if (!anim_parse_chunk_header(data, len, &hdr)) return;
    if (hdr.width != CONFIG_MATRIX_WIDTH || hdr.height != CONFIG_MATRIX_HEIGHT) { anim_set_status(5); return; }
    const uint8_t *px = data + 6; size_t fsz = hdr.width * hdr.height * 3;
    for (int k = 0; k < hdr.count; k++) {
        const uint8_t *frame = px + k * fsz;
        anim_engine_push_frame(frame, fsz);                 // play immediately
        if (s_slot >= 0) {                                   // persist in background
            anim_flash_write(s_slot, frame, fsz);
            s_write_cursor += fsz;
        }
        mbedtls_sha256_update(&s_hash_ctx, frame, fsz);
    }
    anim_set_status(2 /*RECEIVING*/);
}
void anim_handle_play_cmd(uint8_t cmd) {
    if (cmd == 1) { // PLAY: verify hash, commit slot, start loop
        uint8_t digest[32]; mbedtls_sha256_finish(&s_hash_ctx, digest);
        if (memcmp(digest, s_pending_hash, 32) != 0) { anim_set_status(5); return; }
        if (s_slot >= 0) anim_flash_commit(s_slot, s_pending_hash, s_total, CONFIG_ANIM_FPS, 1);
        s_running = true; anim_set_status(4);
    } else if (cmd == 2) { anim_engine_stop(); }
}
```

- [ ] **Step 2: Route the custom-cluster writes in app_main**

In `app_main.cpp` `app_attribute_update_cb`, add before the standard-cluster handling:
```cpp
if (cluster_id == anim::CLUSTER_ID) {
    if (attribute_id == anim::ATTR_TRANSFER_HASH) anim_handle_transfer_hash(val->val.a.b, val->val.a.s);
    else if (attribute_id == anim::ATTR_TRANSFER_META) anim_handle_transfer_meta(val->val.a.b, val->val.a.s);
    else if (attribute_id == anim::ATTR_FRAME_CHUNK) anim_handle_frame_chunk(val->val.a.b, val->val.a.s);
    else if (attribute_id == anim::ATTR_PLAY_CMD) anim_handle_play_cmd(val->val.a.b[0]);
    return ESP_OK;
}
```
(Octet-string values arrive as `val->val.a.b` (buffer) + `val->val.a.s` (size) — verified against `esp_matter_attribute_utils.h`.)

- [ ] **Step 3: Build**

Run: `cd firmware && idf.py build`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add firmware/ && git commit -m "feat(fw): transfer state machine (announce/receive/verify/play)"
```

---

## Phase E — Standard-cluster integration

### Task 12: Layered OnOff/Level/ColorControl

**Files:**
- Modify: `firmware/main/app_driver.cpp` (route standard writes through the animation mode)

**Interfaces:**
- Consumes: `anim_engine_set_brightness`, `anim_engine_stop`, `ws2812_matrix_*`.

- [ ] **Step 1: Route OnOff as master power, Level as brightness, ColorControl as exit-to-static**

In `app_driver.cpp` `app_driver_attribute_update`:
- `OnOff::OnOff` → off: `anim_engine_stop(); ws2812_matrix_clear(m);` on: resume static color.
- `LevelControl::CurrentLevel` → `anim_engine_set_brightness(REMAP_TO_RANGE(val->val.u8, MATTER_BRIGHTNESS, 100));` (keep `led_driver_set_brightness` for static mode).
- `ColorControl` hue/sat/CT/XY → call `anim_engine_stop()` then the existing static `led_driver_*` path (which now writes to the matrix via `ws2812_matrix_fill_rgb`).

Replace `led_driver_*` calls for the WS2812 build with `ws2812_matrix_fill_rgb` (the existing `app_driver_light_set_*` helpers currently call `led_driver_*`; add a thin mapping so static color fills the matrix).

- [ ] **Step 2: Build**

Run: `cd firmware && idf.py build`
Expected: build succeeds.

- [ ] **Step 3: Flash and verify interplay**

Run: `idf.py -p /dev/ttyUSB0 flash monitor`, then via chip-tool:
```bash
./chip-tool onoff off <node> 1       # expect matrix blank
./chip-tool onoff on <node> 1        # expect static color returns
./chip-tool levelcontrol move-to-level 128 0 0 0 <node> 1  # expect brightness scales
./chip-tool colorcontrol move-to-hue-and-saturation 120 200 0 0 0 <node> 1  # expect static color (exits anim)
```
Expected: all behaviors per spec §8.

- [ ] **Step 4: Commit**

```bash
git add firmware/ && git commit -m "feat(fw): layered OnOff/Level/ColorControl integration"
```

---

## Phase F — Attestation

### Task 13: CD, PAI/DAC, factory partition

**Files:**
- Create: `tools/certs/README.md` (documented commands), `firmware/main/certification_declaration/` (generated CD `.der`)
- Modify: `firmware/sdkconfig.defaults`, `firmware/main/CMakeLists.txt`

- [ ] **Step 1: Generate a test CD for VID 0x1618 / PID 0x0001**

```bash
cd ~/.espressif/versions/esp-matter/release-v1.6/connectedhomeip/connectedhomeip
# build host tools if missing
source ./scripts/activate.sh  # or ensure chip-cert on PATH
out/host/chip-cert gen-cd -f 1 -V 0x1618 -p 0x0001 -d 0x0016 \
  -c "CSA00000SWC00000-01" -l 0 -i 0 -n 1 -t 0 \
  -K credentials/test/certification-declaration/Chip-Test-CD-Signing-Key.pem \
  -C credentials/test/certification-declaration/Chip-Test-CD-Signing-Cert.pem \
  -O /path/to/repo/firmware/main/certification_declaration/certification_declaration.der
```

- [ ] **Step 2: Enable the CD + factory providers in sdkconfig.defaults**

```ini
CONFIG_ENABLE_SET_CERT_DECLARATION_API=y
CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER=y
CONFIG_ENABLE_ESP32_DEVICE_INSTANCE_INFO_PROVIDER=y
CONFIG_FACTORY_COMMISSIONABLE_DATA_PROVIDER=y
CONFIG_FACTORY_DEVICE_INSTANCE_INFO_PROVIDER=y
CONFIG_FACTORY_PARTITION_DAC_PROVIDER=y
CONFIG_ENABLE_OTA_REQUESTOR=n
```

- [ ] **Step 3: Register the CD binary in main/CMakeLists.txt**

```cmake
if (CONFIG_ENABLE_SET_CERT_DECLARATION_API)
    target_add_binary_data(${COMPONENT_TARGET} "certification_declaration/certification_declaration.der" BINARY)
endif()
```

- [ ] **Step 4: Generate the factory partition (DAC + PAI + CD + discriminator)**

```bash
python3 -m pip install esp-matter-mfg-tool
esp-matter-mfg-tool --passcode 89674523 --discriminator 2245 \
  -cd firmware/main/certification_declaration/certification_declaration.der \
  -v 0x1618 --vendor-name "Matterize Labs" -p 0x0001 --product-name "WS2812 Animation Light" \
  --hw-ver 1 --hw-ver-str v1.0
```
Note the generated factory `<uuid>.bin` path; flash it with `parttool.py write_partition fctry <uuid>.bin`.

- [ ] **Step 5: Build + flash + commission**

Run: `cd firmware && idf.py build && idf.py -p /dev/ttyUSB0 flash monitor`
Flash the factory bin to `fctry`, then commission with chip-tool and confirm `basicinformation` reports VID `0x1618` / PID `0x0001`.

- [ ] **Step 6: Commit**

```bash
git add firmware/ tools/certs && git commit -m "feat(fw): attestation (CD + factory DAC provider)"
```

---

## Phase G — End-to-end

### Task 14: End-to-end hardware verification

- [ ] **Step 1: Generate payloads from a fixture Lottie**

```bash
cd tools && python lottie2matter.py fixtures/sample.lottie --node-id <node> > /tmp/anim.sh
```
(Add a small `fixtures/sample.lottie` — any dotLottie with a few frames.)

- [ ] **Step 2: Stream + play via chip-tool interactive**

```bash
chip-tool interactive start
# paste the emitted lines (minus ./chip-tool prefix)
```
Expected: matrix plays the animation at 30 fps, smooth, looping.

- [ ] **Step 3: Verify cache hit (no re-transfer)**

Re-paste only the `TransferHash` line + `PlayCmd=PLAY` line.
Expected: device detects hash HIT and plays immediately from flash (no chunk streaming needed).

- [ ] **Step 4: Verify persistence across reboot**

Power-cycle the ESP32, re-paste `PlayCmd=PLAY` with the same hash.
Expected: animation plays from the persisted flash cache.

- [ ] **Step 5: Commit any test fixtures/docs**

```bash
git add tools/fixtures && git commit -m "test(e2e): fixture lottie + verification notes"
```

---

## Self-Review notes (already applied)

- Spec §2 goals → Tasks 4 (tool), 9–11 (stream+cache+hash), 12 (layered clusters), 5–7 (custom cluster + geometry).
- Wire format §6 → Task 2 (encode) + Task 8 (parse) + Task 11 (state machine).
- Flash cache §7.3 → Task 10. Attestation §12 → Task 13. Testing §11 → Tasks 12, 14.
- Types consistent: `FrameChunk`/`TransferMeta`/hash definitions identical between `codec.py` (Task 2) and `anim_codec`/`anim_flash` (Tasks 8/10/11).
