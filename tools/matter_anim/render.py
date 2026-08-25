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
        # Lottie's ip..op-1 semantics: an animation has op - ip frames. Derive
        # the count from the data directly so it is independent of rlottie's
        # undocumented get_totalframe() behavior.
        total = max(0, int(lottie.get("op", 0)) - int(lottie.get("ip", 0)))
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
