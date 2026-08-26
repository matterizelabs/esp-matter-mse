#include "anim_protocol.h"

bool anim_parse_chunk_header(const uint8_t *buf, size_t len, anim_chunk_hdr_t *out) {
    if (!buf || !out || len < 6) return false;
    out->frame_index = (uint16_t)(buf[0] | (buf[1] << 8));
    out->count = buf[2];
    out->width = buf[3];
    out->height = buf[4];
    out->fps = buf[5];
    return true;
}
