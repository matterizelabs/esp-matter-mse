# Matter WS2812 Animation Light — Design Spec

**Date:** 2026-08-25
**Product:** Matterize Labs — VID `0x1618`, PID `0x0001`
**Target:** ESP32 (classic, no PSRAM), flashed via `/dev/ttyUSB0`
**Toolchain:** esp-idf `v6.0.2`, esp-matter `release-v1.6`

---

## 1. Overview

Extend the esp-matter *extended color light* into a device that drives a **48-LED WS2812 matrix (8×6)** and plays **looping animations** sourced from **Lottie** files. The device never parses Lottie; a companion tool renders Lottie into raw WS2812 RGB frames and delivers them over **Matter** to a **custom vendor cluster**. Animations are **cached in flash** (hash-keyed, LRU) and played back at **30 fps**, smooth and glitch-free.

The device remains a standard Matter extended color light (OnOff + LevelControl + ColorControl), so it works with any Matter controller for ordinary lighting.

## 2. Goals & non-goals

**Goals**
- Play looping animations (up to 30 s @ 30 fps = 900 frames) from Lottie sources.
- Stream-and-cache: start displaying as soon as the first frame arrives, while a background task persists frames to flash.
- Hash-keyed cache of the last 5 animations (LRU) so re-playing a known animation needs no re-transfer.
- Smooth, glitch-free, minimal-latency playback.
- Scale to other LED counts/geometries via configuration (no code change).
- Standard Matter light control layered on top (see §8).
- Manufacturable device identity: DAC + PAI + CD.

**Non-goals (v1)**
- Real-time Lottie parsing/rendering on-device.
- Real-time LED-count autodetection (WS2812 is one-way; not feasible).
- Native chip-tool cluster commands / Python controller type bindings (deferred; see §12).

## 3. Constraints & key numbers

| Item | Value |
|---|---|
| LED count | 48 (8 wide × 6 high), single daisy chain |
| Bytes/frame | 48 × 3 = 144 B |
| FPS | 30 |
| Max animation length | 30 s = 900 frames × 144 B = **129,600 B (126.6 KiB)** |
| Cache size | 5 × 128 KiB slots + metadata ≈ 644 KiB → **768 KiB** `anim_cache` partition |
| RAM jitter buffer | 8 frames ≈ 1.2 KB |
| Matter message payload ceiling | ≈ 1100 B (`kMaxIPPacketSizeBytes=1280`, minus headers/tag) |
| Free heap (after Matter+Wi-Fi) | ≈ 150–200 KB |

## 4. System architecture & data flow

```
Lottie JSON ──▶ [lottie2matter.py: render → 8×6 RGB frames → wire format]
                       │  hex payloads
                       ▼
              chip-tool write-by-id (interactive)   [now]
              Python Matter controller              [later]
                       │  Matter over Wi-Fi, custom cluster 0x1618FC01
                       ▼
            ESP32: attribute update callback
                       ├──▶ RAM jitter ring buffer ──▶ playback task (30 fps) ──▶ RMT ──▶ WS2812
                       └──▶ flash-write task ──▶ anim_cache partition (5 slots, LRU)
```

**Invariant:** playback reads *only* from a RAM ring buffer. Two producers fill it: the Matter ingress (cold streaming) or the flash cache (cached playback). Playback is never gated on network or flash latency — this is what guarantees smoothness.

## 5. Matter data model — custom vendor cluster

Cluster ID `0x1618FC01` = `VID << 16 | 0xFC01` (manufacturer-specific: MSB 16 bits = VendorID, LSB 16 bits in `0xFC00–0xFFFE`). Created with the **low-level esp-matter API** (no ZAP codegen):

```c
cluster_t *c = cluster::create(endpoint, 0x1618FC01, CLUSTER_FLAG_SERVER);
attribute_t *a = attribute::create(c, id, ATTRIBUTE_FLAG_WRITABLE, esp_matter_octet_str(buf, len), max_len);
```

Attributes are used for control (not commands) so **chip-tool's `write-by-id` works with zero rebuild** (verified: `write-by-id` accepts `hex:...` octet strings for arbitrary cluster/attribute IDs).

| Attr | ID | Type | Access | Purpose |
|---|---|---|---|---|
| `MatrixWidth` | `0x0000` | uint16 | R | geometry (from Kconfig) |
| `MatrixHeight` | `0x0001` | uint16 | R | geometry |
| `PixelCount` | `0x0002` | uint16 | R | = W×H |
| `Serpentine` | `0x0003` | bool | R | index→grid mapping |
| `CachedAnimations` | `0x0004` | octet string | R | concatenated 32-B hashes of cached animations (tool reads for hit/miss) |
| `TransferHash` | `0x0005` | octet string(32) | W | announce target animation hash |
| `TransferMeta` | `0x0006` | octet string(6) | W | `[total_frames u16][fps u8][loop u8][width u8][height u8]` |
| `FrameChunk` | `0x0007` | octet string(≤~1 KB) | W | N frames (see §6) |
| `TransferStatus` | `0x0008` | enum8 | R | IDLE=0, ANNOUNCED=1, RECEIVING=2, VERIFYING=3, READY=4, ERROR=5 |
| `PlayCmd` | `0x0009` | enum8 | W | NONE=0, PLAY=1, STOP=2, CLEAR_CACHE=3 (write triggers action) |
| `ActiveAnimation` | `0x000A` | octet string(32) | R | hash currently playing (zero = none) |

The cluster lives on the **same endpoint** as the extended color light (endpoint 1), with `priv_data` pointing at the animation engine.

## 6. Wire format & transfer protocol

**FrameChunk payload** (all fields little-endian):

```
[frame_index u16][count u8][width u8][height u8][fps u8][RGB × (count × width × height)]
```

`count` = frames in this chunk (packed so total ≤ ~1 KB: up to 6 frames of 48 LEDs). The device validates `width/height` against its own config and rejects mismatches (e.g., a 96-LED payload vs a 48-LED config) instead of mis-painting.

**Hash:** SHA-256 over the concatenation of all frames in order (144 B each).

**Protocol state machine:**

1. Tool reads `CachedAnimations`. If target hash present → **HIT** → jump to 5.
2. Tool writes `TransferHash` + `TransferMeta` → device: `IDLE→ANNOUNCED` (miss) or `READY` (hit).
3. Tool streams `FrameChunk` writes. Device copies each into the RAM jitter buffer (playback starts on first chunk) **and** queues it to the flash-write task. Status `RECEIVING`.
4. After the final chunk, device recomputes SHA-256 over received frames → `VERIFYING` → match: commit slot, `READY`; mismatch: discard slot, `ERROR`.
5. Tool writes `PlayCmd=PLAY` → device plays from flash at 30 fps (loop if `loop` flag).
6. `PlayCmd=STOP` → stop, return to static color (or off, per OnOff).
7. `PlayCmd=CLEAR_CACHE` → erase all slots.

## 7. On-device architecture

### 7.1 Tasks
- **Matter task** (esp-matter's own): the attribute update callback (PRE_UPDATE) handles `FrameChunk`/`TransferHash`/`TransferMeta`/`PlayCmd`. For `FrameChunk`, it copies the payload into the jitter ring buffer and a flash-write queue.
- **Playback task** (high priority, 30 fps tick): pops next frame from ring buffer, applies master brightness (§8), writes 48 pixels via `led_strip`, refreshes RMT. Wraps to frame 0 for loop.
- **Flash-write task** (low priority): drains the flash-write queue, writes to `anim_cache` (page-aligned, erase-before-write), maintains the slot table + LRU + hash verification.

### 7.2 Drivers
New `ws2812_matrix` component (uses `led_strip`/RMT directly) with `fill_rgb()`, `set_frame(rgb[48])`, `show()`. The existing esp-matter `led_driver` (single-LED gpio) is **replaced** by this driver for this build. LED count and GPIO come from Kconfig.

### 7.3 Flash cache layout (`anim_cache`, 768 KB)
- Metadata sector (4 KB): magic, version, 5 slot descriptors `{hash[32], frame_count, fps, loop, flags, LRU stamp, state}`.
- 5 × 128 KiB (131,072 B) fixed slots = 640 KiB. A max animation (129,600 B = 126.6 KiB) fits in one 128 KiB slot.
- Metadata 4 KiB + 640 KiB = 644 KiB < 768 KiB partition (headroom for wear/alignment).

### 7.4 Configuration (Kconfig)
`CONFIG_WS2812_GPIO` (default 5), `CONFIG_MATRIX_WIDTH` (8), `CONFIG_MATRIX_HEIGHT` (6), `CONFIG_MATRIX_SERPENTINE` (y), `CONFIG_ANIM_FPS` (30). Geometry is exposed via the read-only cluster attributes. (NVS runtime override of geometry deferred to v1.x.)

## 8. Standard-cluster integration (layered)

- **OnOff** = master power. Off blanks the matrix and stops playback; On resumes last animation (or static color).
- **LevelControl** = live master brightness multiplier (0–100 %) applied to animation output.
- **ColorControl** (hue/sat/color-temp/XY) = **exits** animation mode and returns to static-color fill.

Implementation: the existing `app_driver_attribute_update` routes standard-cluster writes to the matrix driver's static path; the animation engine holds the "current mode" (STATIC vs ANIMATION) and arbitrates.

## 9. Companion tool — `lottie2matter.py`

A **pure local CLI** — no browser, no network/API, no external service. Point it at a local file; it emits the exact wire payload.

**Input (auto-detected, any one):**
- `.lottie` (dotLottie ZIP archive) — extract via stdlib `zipfile`, read `manifest.json`, locate the animation JSON.
- Lottie `.json`
- optimized/minified Lottie JSON

**Pipeline:**
1. Geometry from CLI args (`--width 8 --height 6 --serpentine`), with defaults; optionally read from the device over Matter when online.
2. Render with **`rlottie-python` 1.3.8** (local, native, bundled rlottie — no browser). `render_pillow_frame(i, width=W, height=H)` returns a PIL `Image` at target size, for each frame up to 900 (30 s @ 30 fps).
3. Serpentine-map each frame to chain order → 144-B frames.
4. Compute **SHA-256** over the concatenated frames (matches the device's verification hash).

**Output (deterministic, stdout):**
- animation hash (hex)
- `TransferMeta` hex (`[total_frames u16][fps u8][loop u8][width u8][height u8]`)
- `FrameChunk` hex payloads, packed ≤ ~1 KB each (≤ 6 frames)
- optionally, ready-to-paste `chip-tool write-by-id` command lines

Later (v2): the tool streams directly as a Matter controller (Python `chip` bindings), replacing chip-tool.

**Rendering decision (resolved):** `rlottie-python` only — native, fast, feature-complete, verified wheel installs on this box. No browser-based fallback (per requirement).

## 10. Error handling

- **Hash mismatch** after transfer → `ERROR`, slot not committed, no playback.
- **Buffer overrun** (ingress < drain) → stop, `ERROR`, fall back to static color (never tear a frame).
- **Bad frame header / wrong count** → reject chunk, `ERROR`.
- **Flash full / erase failure** → evict LRU, retry; on failure `ERROR`.
- **Config mismatch** (payload width/height ≠ device) → reject, `ERROR`.

## 11. Testing

- **Unit:** frame header encode/decode; SHA-256 cache keying + LRU eviction; brightness multiply; serpentine mapping.
- **Hardware:** single-frame write → correct 8×6 pattern; short clip @ 30 fps → smooth, no glitches; hash hit → instant play; power-cycle → cache persists; standard-cluster interplay (on/off/dim/color-exit); 30 s worst-case animation loop.
- **End-to-end:** `lottie2matter.py` → chip-tool interactive → device.
- **Scalability:** rebuild with a different W×H and confirm tool + device adapt via config only.

## 12. Manufacturing & attestation

- **CD:** `chip-cert gen-cd -f 1 -V 0x1618 -p 0x0001 ...` (test CD-signing key for dev).
- **PAI + DAC:** `esp-matter-mfg-tool -n <count> -v 0x1618 -p 0x0001 -cd <CD.der> ...` → factory partition.
- **Firmware config:** DAC provider (`CONFIG_SEC_CERT_DAC_PROVIDER=y`) + factory data providers; CD embedded or in factory partition.
- **Dev vs production:** use test PAA + test CD-signing key now; real CSA-signed PAI/CD when certifying.

## 13. Build/config

- esp-idf `v6.0.2` (set `IDF_PATH` to `~/.espressif/versions/esp-idf/v6.0.2`), esp-matter `release-v1.6`.
- Target `esp32`.
- New board profile (`led_type=ws2812`) or override `ESP_MATTER_DEVICE_PATH`; GPIO from Kconfig.
- Custom `partitions.csv` sized for 4 MB flash: app, factory (attestation), nvs, `anim_cache` (768 KB), otadata. **OTA A/B is dropped/shrunk for v1** to make room for `anim_cache` (partition sizing is a plan-time task).

## 14. Future work (explicitly deferred)

- ZAP XML template + `zap_regen_all.py` → native chip-tool commands + Python/Android/Darwin typed bindings for the custom cluster.
- NVS runtime override of geometry (Kconfig is the v1 source of truth).
- RLE compression of cached animations to shrink flash usage.
- Procedural (non-Lottie) ambient effects generated on-device.
