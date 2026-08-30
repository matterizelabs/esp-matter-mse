from matter_stream import codec


def build_commands(node_id, hash_bytes, meta_bytes, chunks):
    cid = f"0x{codec.CLUSTER_ID:08X}"
    cmds = []
    cmds.append(f"./chip-tool any write-by-id {cid} 0x{codec.ATTR_TRANSFER_HASH:04X} hex:{hash_bytes.hex()} {node_id} 1")
    cmds.append(f"./chip-tool any write-by-id {cid} 0x{codec.ATTR_TRANSFER_META:04X} hex:{meta_bytes.hex()} {node_id} 1")
    for ch in chunks:
        cmds.append(f"./chip-tool any write-by-id {cid} 0x{codec.ATTR_FRAME_CHUNK:04X} hex:{ch.hex()} {node_id} 1")
    cmds.append(f"./chip-tool any write-by-id {cid} 0x{codec.ATTR_PLAY_CMD:04X} hex:01 {node_id} 1")
    return cmds
