import hashlib
import struct

from matter_stream.protocol import (
    ATTR_FRAME_CHUNK,
    ATTR_PLAY_CMD,
    ATTR_TRANSFER_HASH,
    ATTR_TRANSFER_META,
    CHUNK_HEADER_FMT,
    CLUSTER_ID,
    MAX_CHUNK_BYTES,
    META_FMT,
)

__all__ = [
    "ATTR_FRAME_CHUNK",
    "ATTR_PLAY_CMD",
    "ATTR_TRANSFER_HASH",
    "ATTR_TRANSFER_META",
    "CLUSTER_ID",
]


def serpentine_chain_order(width, height, serpentine=True):
    # physical wiring: column-major, starts top-right, snakes down then up each column
    order = []
    for x in range(width - 1, -1, -1):
        down = (not serpentine) or ((width - 1 - x) % 2 == 0)
        col = range(height) if down else range(height - 1, -1, -1)
        for y in col:
            order.append((x, y))
    return order


def frame_to_bytes(frame_pixels, order):
    # frame_pixels[y][x] -> (r,g,b) tuple
    out = bytearray()
    for x, y in order:
        r, g, b = frame_pixels[y][x]
        out += bytes((r, g, b))
    return bytes(out)


def encode_chunk(frame_index, frames, width, height, fps):
    header = struct.pack(CHUNK_HEADER_FMT, frame_index, len(frames), width, height, fps)
    return header + b"".join(frames)


def encode_meta(total_frames, fps, loop, width, height):
    return struct.pack(META_FMT, total_frames, fps, loop, width, height)


def stream_hash(frames):
    return hashlib.sha256(b"".join(frames)).digest()


def pack_chunks(frames, width, height, fps):
    bytes_per_frame = width * height * 3
    max_count = (MAX_CHUNK_BYTES - 6) // bytes_per_frame
    chunks = []
    for i in range(0, len(frames), max_count):
        batch = frames[i : i + max_count]
        chunks.append(encode_chunk(i, batch, width, height, fps))
    return chunks
