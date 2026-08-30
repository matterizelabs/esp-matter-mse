import argparse
import sys

from matter_stream import codec
from matter_stream.cli import build_commands


def main(argv=None):
    p = argparse.ArgumentParser(prog="chase.py", description="Generate a one-LED-at-a-time chase to verify the physical LED chain order.")
    p.add_argument("--leds", type=int, default=30, help="number of LEDs to chase (default 30, max at 1s/led within cache slot)")
    p.add_argument("--seconds", type=float, default=1.0, help="hold time per LED")
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--width", type=int, default=8)
    p.add_argument("--height", type=int, default=6)
    p.add_argument("--node-id", type=int, default=1)
    p.add_argument("--color", default="ffffff", help="RGB hex of the lit pixel")
    p.add_argument("--prefix", default="./chip-tool", help="command prefix (e.g. ct)")
    args = p.parse_args(argv)

    total = args.width * args.height
    if args.leds > total:
        args.leds = total
    hold = max(1, int(args.fps * args.seconds))
    n_frames = args.leds * hold
    slot_bytes = n_frames * total * 3
    if slot_bytes > 128 * 1024:
        print(f"error: {n_frames} frames = {slot_bytes} bytes exceeds 128 KiB cache slot", file=sys.stderr)
        return 2

    r = int(args.color[0:2], 16)
    g = int(args.color[2:4], 16)
    b = int(args.color[4:6], 16)

    frames = []
    for k in range(args.leds):
        base = bytearray(total * 3)
        base[k * 3] = r
        base[k * 3 + 1] = g
        base[k * 3 + 2] = b
        for _ in range(hold):
            frames.append(bytes(base))

    h = codec.stream_hash(frames)
    meta = codec.encode_meta(n_frames, args.fps, 1, args.width, args.height)
    chunks = codec.pack_chunks(frames, args.width, args.height, args.fps)

    for c in build_commands(args.node_id, h, meta, chunks):
        print(c.replace("./chip-tool", args.prefix))
    print(f"# chase: leds=0..{args.leds-1} hold={hold}frames({args.seconds}s) frames={n_frames} chunks={len(chunks)} hash={h.hex()}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
