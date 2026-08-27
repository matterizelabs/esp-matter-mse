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
