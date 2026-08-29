#pragma once
#include <esp_err.h>
#include <esp_matter.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ws2812_matrix.h"
#include "anim_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

bool anim_parse_chunk_header(const uint8_t *buf, size_t len, anim_chunk_hdr_t *out);

#ifdef __cplusplus
}
#endif

#define ANIM_SLOT_COUNT 5
#define ANIM_SLOT_SIZE (128 * 1024)
#define ANIM_MAX_FRAMES 900

esp_err_t anim_flash_init(void);
int  anim_flash_find(const uint8_t hash[32]);
int  anim_flash_alloc_slot(void);
esp_err_t anim_flash_write(int slot, uint32_t offset, const uint8_t *data, size_t len);
esp_err_t anim_flash_commit(int slot, const uint8_t hash[32], uint16_t total_frames, uint8_t fps, uint8_t loop);
esp_err_t anim_flash_abort(int slot);
esp_err_t anim_flash_clear_all(void);
esp_err_t anim_flash_read_frame(int slot, uint32_t frame_index, uint8_t *out, size_t out_len);
esp_err_t anim_flash_get_slot_info(int slot, uint16_t *total_frames, uint8_t *fps, uint8_t *loop);
esp_err_t anim_flash_get_slot_hash(int slot, uint8_t *out, size_t len);

esp_err_t anim_engine_init(ws2812_matrix_handle_t m);
bool anim_engine_push_frame(const uint8_t *chain_rgb, size_t len);
void anim_engine_set_brightness(uint8_t pct);
void anim_engine_stop(void);
bool anim_engine_is_running(void);

void anim_handle_frame_chunk(const uint8_t *data, size_t len);
void anim_handle_transfer_hash(const uint8_t *hash, size_t len);
void anim_handle_transfer_meta(const uint8_t *meta, size_t len);
void anim_handle_play_cmd(uint8_t cmd);
void anim_set_status(uint8_t status);

#ifdef __cplusplus
namespace anim {
constexpr uint32_t CLUSTER_ID = ANIM_CLUSTER_ID;
constexpr uint32_t ATTR_MATRIX_WIDTH = ANIM_ATTR_MATRIX_WIDTH;
constexpr uint32_t ATTR_MATRIX_HEIGHT = ANIM_ATTR_MATRIX_HEIGHT;
constexpr uint32_t ATTR_PIXEL_COUNT = ANIM_ATTR_PIXEL_COUNT;
constexpr uint32_t ATTR_SERPENTINE = ANIM_ATTR_SERPENTINE;
constexpr uint32_t ATTR_CACHED = ANIM_ATTR_CACHED;
constexpr uint32_t ATTR_TRANSFER_HASH = ANIM_ATTR_TRANSFER_HASH;
constexpr uint32_t ATTR_TRANSFER_META = ANIM_ATTR_TRANSFER_META;
constexpr uint32_t ATTR_FRAME_CHUNK = ANIM_ATTR_FRAME_CHUNK;
constexpr uint32_t ATTR_STATUS = ANIM_ATTR_STATUS;
constexpr uint32_t ATTR_PLAY_CMD = ANIM_ATTR_PLAY_CMD;
constexpr uint32_t ATTR_ACTIVE = ANIM_ATTR_ACTIVE;
esp_matter::cluster_t *anim_cluster_create(esp_matter::endpoint_t *ep);
}
#endif
