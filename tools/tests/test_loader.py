import json, zipfile, os
from matter_anim import loader

def _write_lottie_json(tmp_path, data):
    p = tmp_path / "a.json"
    p.write_text(json.dumps(data))
    return str(p)

def _write_dotlottie(tmp_path, lottie_dict):
    p = tmp_path / "a.lottie"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("manifest.json", json.dumps({"animations": [{"id": "anim1"}]}))
        z.writestr("animations/anim1.json", json.dumps(lottie_dict))
    return str(p)

def test_loads_plain_json(tmp_path):
    d = {"v": "5.7.4", "fr": 30, "ip": 0, "op": 60, "layers": []}
    out = loader.load_animation(_write_lottie_json(tmp_path, d))
    assert out["v"] == "5.7.4"

def test_loads_dotlottie(tmp_path):
    d = {"v": "5.7.4", "fr": 30, "ip": 0, "op": 60, "layers": []}
    out = loader.load_animation(_write_dotlottie(tmp_path, d))
    assert out["v"] == "5.7.4"

def test_rejects_unknown_extension(tmp_path):
    p = tmp_path / "a.txt"
    p.write_text("x")
    import pytest
    with pytest.raises(ValueError):
        loader.load_animation(str(p))
