#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint16_t frame_index;
    uint8_t count;
    uint8_t width;
    uint8_t height;
    uint8_t fps;
} anim_chunk_hdr_t;

bool anim_parse_chunk_header(const uint8_t *buf, size_t len, anim_chunk_hdr_t *out);
