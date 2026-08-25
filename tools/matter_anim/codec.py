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
