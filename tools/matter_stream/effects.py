"""Procedural LED effect generators for the WS2812 matrix.

Each generator returns a list of frame byte strings in wire (chain) order:
``width * height * 3`` bytes per frame, where chain index ``i`` maps to the
physical LED at ``codec.serpentine_chain_order(width, height)[i]``.

All generators are deterministic for a given set of parameters, so the same
effect + parameters always produces the same frame stream and therefore the
same SHA-256 (flash cache dedup keeps working).
"""

import colorsys
import math
import random

from matter_stream import codec


def _blank(width, height):
    return bytearray(width * height * 3)


def _idx_map(width, height, serpentine=True):
    order = codec.serpentine_chain_order(width, height, serpentine)
    return {xy: i for i, xy in enumerate(order)}


def _hex_rgb(value):
    value = value.lstrip("#")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def _hsv(hue, sat=1.0, val=1.0):
    r, g, b = colorsys.hsv_to_rgb((hue % 360) / 360.0, sat, val)
    return (round(r * 255), round(g * 255), round(b * 255))


def _put(buf, idx, rgb):
    buf[idx * 3:idx * 3 + 3] = bytes(rgb)
    return buf


def _fill(buf, rgb):
    for i in range(0, len(buf), 3):
        buf[i:i + 3] = bytes(rgb)
    return buf


def _seconds(fps, seconds):
    return max(1, int(seconds * fps))


def solid(color="#ffffff", seconds=1.0, width=8, height=6, fps=30, **kw):
    rgb = _hex_rgb(color)
    frame = bytes(_fill(_blank(width, height), rgb))
    return [frame] * _seconds(fps, seconds)


def chase(color="#ffffff", speed=30.0, size=1, tail=0, seconds=None,
          width=8, height=6, fps=30, **kw):
    count = width * height
    if seconds is None:
        seconds = count / speed
    total = _seconds(fps, seconds)
    step = speed / fps
    rgb = _hex_rgb(color)
    frames = []
    for t in range(total):
        head = int(t * step) % count
        buf = _blank(width, height)
        for k in range(size):
            _put(buf, (head - k) % count, rgb)
        if tail:
            for k in range(size, size + tail):
                fade = max(0.0, 1.0 - (k - size + 1) / (tail + 1))
                _put(buf, (head - k) % count, tuple(int(c * fade) for c in rgb))
        frames.append(bytes(buf))
    return frames


def comet(color="#ffffff", speed=15.0, tail=8, seconds=None,
          width=8, height=6, fps=30, **kw):
    count = width * height
    if seconds is None:
        seconds = count / speed
    total = _seconds(fps, seconds)
    step = speed / fps
    rgb = _hex_rgb(color)
    frames = []
    for t in range(total):
        head = int(t * step) % count
        buf = _blank(width, height)
        for k in range(tail + 1):
            fade = (tail - k) / tail if tail else 1.0
            _put(buf, (head - k) % count, tuple(int(c * fade) for c in rgb))
        frames.append(bytes(buf))
    return frames


def pulse(color="#ffffff", period=2.0, width=8, height=6, fps=30, **kw):
    total = _seconds(fps, period)
    rgb = _hex_rgb(color)
    frames = []
    for t in range(total):
        v = 0.5 * (1.0 - math.cos(2.0 * math.pi * t / total))
        c = tuple(int(ch * v) for ch in rgb)
        frames.append(bytes(_fill(_blank(width, height), c)))
    return frames


def rainbow(seconds=6.0, spatial=False, width=8, height=6, fps=30, **kw):
    count = width * height
    total = _seconds(fps, seconds)
    if spatial:
        buf = _blank(width, height)
        for i in range(count):
            _put(buf, i, _hsv(i * 360.0 / count))
        return [bytes(buf)] * total
    frames = []
    for t in range(total):
        c = _hsv(t * 360.0 / total)
        frames.append(bytes(_fill(_blank(width, height), c)))
    return frames


def sparkle(color="#ffffff", density=0.25, seconds=6.0, seed=0,
            width=8, height=6, fps=30, **kw):
    rng = random.Random(seed)
    count = width * height
    rgb = _hex_rgb(color)
    frames = []
    for _ in range(_seconds(fps, seconds)):
        buf = _blank(width, height)
        for i in range(count):
            if rng.random() < density:
                _put(buf, i, rgb)
        frames.append(bytes(buf))
    return frames


def wipe(color="#ffffff", direction="ltr", step=1, width=8, height=6, fps=30, **kw):
    imap = _idx_map(width, height, True)
    if direction in ("ltr", "rtl"):
        cols = range(width) if direction == "ltr" else range(width - 1, -1, -1)
        seq = [(x, y) for x in cols for y in range(height)]
    elif direction in ("ttb", "btt"):
        rows = range(height) if direction == "ttb" else range(height - 1, -1, -1)
        seq = [(x, y) for y in rows for x in range(width)]
    else:
        raise ValueError(direction)
    rgb = _hex_rgb(color)
    count = width * height
    frames = []
    filled = 0
    while filled < count:
        buf = _blank(width, height)
        for x, y in seq[:filled + step]:
            _put(buf, imap[(x, y)], rgb)
        frames.append(bytes(buf))
        filled += step
    return frames


def strobe(color="#ffffff", rate=4.0, width=8, height=6, fps=30, **kw):
    on = max(1, int(fps / (2.0 * rate)))
    rgb = _hex_rgb(color)
    on_frame = bytes(_fill(_blank(width, height), rgb))
    off_frame = bytes(_blank(width, height))
    return [on_frame] * on + [off_frame] * on


def fire(seconds=6.0, seed=0, width=8, height=6, fps=30, **kw):
    rng = random.Random(seed)
    count = width * height
    levels = [rng.random() for _ in range(count)]
    frames = []
    for _ in range(_seconds(fps, seconds)):
        buf = _blank(width, height)
        for i in range(count):
            levels[i] = min(1.0, max(0.0, levels[i] + rng.uniform(-0.35, 0.35)))
            v = levels[i]
            _put(buf, i, _hsv(60.0 * v, 1.0, min(1.0, v * 1.3)))
        frames.append(bytes(buf))
    return frames


EFFECTS = {
    "solid": solid,
    "chase": chase,
    "comet": comet,
    "pulse": pulse,
    "rainbow": rainbow,
    "sparkle": sparkle,
    "wipe": wipe,
    "strobe": strobe,
    "fire": fire,
}
