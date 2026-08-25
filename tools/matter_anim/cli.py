import argparse, sys
from matter_anim import loader, render, codec

def build_commands(node_id, hash_bytes, meta_bytes, chunks):
    cid = f"0x{codec.CLUSTER_ID:08X}"
    cmds = []
    cmds.append(f"./chip-tool any write-by-id {cid} 0x{codec.ATTR_TRANSFER_HASH:04X} hex:{hash_bytes.hex()} {node_id} 1")
    cmds.append(f"./chip-tool any write-by-id {cid} 0x{codec.ATTR_TRANSFER_META:04X} hex:{meta_bytes.hex()} {node_id} 1")
    for ch in chunks:
        cmds.append(f"./chip-tool any write-by-id {cid} 0x{codec.ATTR_FRAME_CHUNK:04X} hex:{ch.hex()} {node_id} 1")
    cmds.append(f"./chip-tool any write-by-id {cid} 0x{codec.ATTR_PLAY_CMD:04X} hex:01 {node_id} 1")
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
