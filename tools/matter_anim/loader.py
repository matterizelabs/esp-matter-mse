import json, os, zipfile

SUPPORTED = (".lottie", ".json")

def _extract_dotlottie(path: str) -> dict:
    with zipfile.ZipFile(path) as z:
        manifest = json.loads(z.read("manifest.json"))
        anim = manifest["animations"][0]
        anim_id = anim.get("id") if isinstance(anim, dict) else anim
        name = f"animations/{anim_id}.json"
        if name not in z.namelist():
            # fall back to the first animations/*.json
            name = next(n for n in z.namelist() if n.startswith("animations/") and n.endswith(".json"))
        return json.loads(z.read(name))

def load_animation(path: str) -> dict:
    ext = os.path.splitext(path)[1].lower()
    if ext == ".lottie":
        return _extract_dotlottie(path)
    if ext == ".json":
        with open(path) as f:
            return json.load(f)
    raise ValueError(f"unsupported input: {path!r}; expected .lottie or .json")
