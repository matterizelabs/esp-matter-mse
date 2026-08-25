#pragma once
#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>
#define ANIM_SLOT_COUNT 5
#define ANIM_SLOT_SIZE (128 * 1024)
#define ANIM_MAX_FRAMES 900
esp_err_t anim_flash_init(void);
int  anim_flash_find(const uint8_t hash[32]);
int  anim_flash_alloc_slot(void);
esp_err_t anim_flash_erase(int slot);
esp_err_t anim_flash_write(int slot, uint32_t offset, const uint8_t *data, size_t len);
esp_err_t anim_flash_commit(int slot, const uint8_t hash[32], uint16_t total_frames, uint8_t fps, uint8_t loop);
esp_err_t anim_flash_read_frame(int slot, uint32_t frame_index, uint8_t *out, size_t out_len);
esp_err_t anim_flash_get_slot_info(int slot, uint16_t *total_frames, uint8_t *fps, uint8_t *loop);
