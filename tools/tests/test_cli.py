from matter_anim import cli

def test_build_commands_sequence():
    node = 1234
    h = bytes.fromhex("ab" * 32)
    meta = bytes(6)
    chunks = [bytes.fromhex("cd" * 870)]
    cmds = cli.build_commands(node, h, meta, chunks)
    assert cmds[0] == f"./chip-tool any write-by-id 0x1618FC01 0x0005 hex:{h.hex()} 1234 1"
    assert cmds[1] == f"./chip-tool any write-by-id 0x1618FC01 0x0006 hex:{meta.hex()} 1234 1"
    assert cmds[2] == f"./chip-tool any write-by-id 0x1618FC01 0x0007 hex:{chunks[0].hex()} 1234 1"
    assert cmds[-1] == f"./chip-tool any write-by-id 0x1618FC01 0x0009 hex:01 1234 1"

def test_main_end_to_end(tmp_path, capsys):
    import json, subprocess, sys
    lottie = {"v":"5.7.4","fr":30,"ip":0,"op":2,"w":16,"h":16,
              "layers":[{"ty":4,"ind":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},
              "p":{"a":0,"k":[8,8,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},
              "shapes":[{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[16,16]},"nm":"r","d":1}],
              "ip":0,"op":2,"st":0,"bm":0}]}
    p = tmp_path / "m.json"
    p.write_text(json.dumps(lottie))
    rc = cli.main([str(p), "--node-id", "7"])
    out = capsys.readouterr().out
    assert rc == 0
    assert "any write-by-id 0x1618FC01 0x0005 hex:" in out
    assert out.count("0x0007 hex:") >= 1
    assert "0x0009 hex:01 7 1" in out
