import importlib.util
import json
import os


def _root():
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _load_generator():
    gen_path = os.path.join(_root(), "tools", "generate_wire.py")
    spec = importlib.util.spec_from_file_location("generate_wire", gen_path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_wire_contract_generated_files_are_in_sync():
    root = _root()
    gen = _load_generator()
    with open(os.path.join(root, "shared", "wire_contract.json")) as f:
        schema = json.load(f)
    c_src, py_src = gen.generate(schema)

    with open(os.path.join(root, "components", "animation", "include", "anim_protocol.h")) as f:
        assert f.read() == c_src, "anim_protocol.h is stale; run tools/generate_wire.py"

    with open(os.path.join(root, "tools", "matter_anim", "protocol.py")) as f:
        assert f.read() == py_src, "protocol.py is stale; run tools/generate_wire.py"
