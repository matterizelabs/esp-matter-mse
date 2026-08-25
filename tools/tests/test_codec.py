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
