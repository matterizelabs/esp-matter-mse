from matter_anim import effects


FRAME_BYTES = 8 * 6 * 3


def test_registry_has_catalog():
    for name in ("solid", "chase", "comet", "pulse", "rainbow", "sparkle", "wipe", "strobe", "fire"):
        assert name in effects.EFFECTS


def test_all_effects_emit_valid_frames():
    for name, fn in effects.EFFECTS.items():
        frames = fn()
        assert frames, name
        assert all(len(f) == FRAME_BYTES for f in frames), name


def test_solid_is_uniform():
    frames = effects.solid(color="#ff0000", seconds=0.2, fps=10)
    assert len(frames) == 2
    assert frames[0] == bytes([255, 0, 0]) * 48


def test_chase_lights_one_pixel():
    frames = effects.chase(color="#ffffff", speed=48.0, seconds=1.0, fps=48, size=1)
    assert len(frames) == 48
    for f in frames:
        lit = sum(1 for i in range(0, len(f), 3) if f[i:i + 3] != b"\x00\x00\x00")
        assert lit == 1


def test_deterministic_effects():
    assert effects.sparkle(seed=7) == effects.sparkle(seed=7)
    assert effects.fire(seed=7) == effects.fire(seed=7)
    assert effects.sparkle(seed=7) != effects.sparkle(seed=8)


def test_wipe_fills_left_to_right():
    frames = effects.wipe(color="#ff0000", direction="ltr", step=1)
    assert len(frames) == 48
    assert frames[-1] == bytes([255, 0, 0]) * 48
    # first frame lights exactly one pixel (top-right column, chain index 0)
    assert frames[0].count(b"\xff\x00\x00") == 1
