#pragma once
#include <esp_err.h>
#include "ws2812_matrix.h"
esp_err_t anim_engine_init(ws2812_matrix_handle_t m);
bool anim_engine_push_frame(const uint8_t *chain_rgb, size_t len);
void anim_engine_set_brightness(uint8_t pct);
void anim_engine_stop(void);
