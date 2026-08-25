#pragma once
#include <esp_err.h>
#include "ws2812_matrix.h"
esp_err_t anim_engine_init(ws2812_matrix_handle_t m);
bool anim_engine_push_frame(const uint8_t *chain_rgb, size_t len);
void anim_engine_set_brightness(uint8_t pct);
void anim_engine_stop(void);
bool anim_engine_is_running(void);

/* Transfer state machine (announce -> stream -> verify/play) */
void anim_handle_frame_chunk(const uint8_t *data, size_t len);
void anim_handle_transfer_hash(const uint8_t *hash, size_t len);
void anim_handle_transfer_meta(const uint8_t *meta, size_t len);
void anim_handle_play_cmd(uint8_t cmd);
void anim_set_status(uint8_t status);
