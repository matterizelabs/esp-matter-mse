import argparse
import sys

from matter_anim import cli, codec, effects


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="effects.py",
        description="Generate a WS2812 LED effect and emit Matter write-by-id commands.",
    )
    p.add_argument("effect", choices=sorted(effects.EFFECTS))
    p.add_argument("--color", default="#ffffff")
    p.add_argument("--seconds", type=float, default=None, help="effect duration (chase/comet derive it from speed when omitted)")
    p.add_argument("--speed", type=float, default=None, help="chain positions per second (chase/comet)")
    p.add_argument("--size", type=int, default=1, help="chase block size")
    p.add_argument("--tail", type=int, default=None, help="fading trail length (chase/comet)")
    p.add_argument("--period", type=float, default=2.0, help="pulse period in seconds")
    p.add_argument("--density", type=float, default=0.25, help="sparkle probability per pixel per frame")
    p.add_argument("--direction", default="ltr", choices=("ltr", "rtl", "ttb", "btt"))
    p.add_argument("--step", type=int, default=1, help="wipe pixels per frame")
    p.add_argument("--rate", type=float, default=4.0, help="strobe flashes per second")
    p.add_argument("--spatial", action="store_true", help="rainbow as a spatial gradient instead of temporal")
    p.add_argument("--seed", type=int, default=0, help="random seed (sparkle/fire)")
    p.add_argument("--node-id", type=int, default=1)
    p.add_argument("--width", type=int, default=8)
    p.add_argument("--height", type=int, default=6)
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--no-serpentine", dest="serpentine", action="store_false", default=True)
    p.add_argument("--prefix", default="./chip-tool")
    args = p.parse_args(argv)

    kw = dict(width=args.width, height=args.height, fps=args.fps, color=args.color)
    name = args.effect
    if name in ("chase", "comet"):
        kw["speed"] = args.speed or (15.0 if name == "comet" else 30.0)
    if name == "chase":
        kw["size"] = args.size
        kw["tail"] = args.tail or 0
    if name == "comet":
        kw["tail"] = args.tail or 8
    if name == "pulse":
        kw["period"] = args.period
    if name == "rainbow":
        kw["spatial"] = args.spatial
    if name == "sparkle":
        kw["density"] = args.density
        kw["seed"] = args.seed
    if name == "wipe":
        kw["direction"] = args.direction
        kw["step"] = args.step
    if name == "strobe":
        kw["rate"] = args.rate
    if name == "fire":
        kw["seed"] = args.seed
    if args.seconds is not None:
        kw["seconds"] = args.seconds

    frames = effects.EFFECTS[name](**kw)
    if not frames:
        print("error: effect produced no frames", file=sys.stderr)
        return 2

    h = codec.animation_hash(frames)
    meta = codec.encode_meta(len(frames), args.fps, 1, args.width, args.height)
    chunks = codec.pack_chunks(frames, args.width, args.height, args.fps)

    for c in cli.build_commands(args.node_id, h, meta, chunks):
        print(c.replace("./chip-tool", args.prefix))
    print(f"# effect={name} frames={len(frames)} chunks={len(chunks)} hash={h.hex()}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
