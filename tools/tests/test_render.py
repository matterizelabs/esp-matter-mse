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
