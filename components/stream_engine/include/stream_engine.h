#pragma once
#include <esp_err.h>
#include <esp_matter.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ws2812_matrix.h"
#include "stream_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

bool stream_parse_chunk_header(const uint8_t *buf, size_t len, stream_chunk_hdr_t *out);

#ifdef __cplusplus
}
#endif

#define STREAM_SLOT_COUNT 5
#define STREAM_SLOT_SIZE (128 * 1024)
#define STREAM_MAX_FRAMES 900

esp_err_t stream_flash_init(void);
int  stream_flash_find(const uint8_t hash[32]);
int  stream_flash_alloc_slot(void);
esp_err_t stream_flash_write(int slot, uint32_t offset, const uint8_t *data, size_t len);
esp_err_t stream_flash_commit(int slot, const uint8_t hash[32], uint16_t total_frames, uint8_t fps, uint8_t loop);
esp_err_t stream_flash_abort(int slot);
esp_err_t stream_flash_clear_all(void);
esp_err_t stream_flash_read_frame(int slot, uint32_t frame_index, uint8_t *out, size_t out_len);
esp_err_t stream_flash_get_slot_info(int slot, uint16_t *total_frames, uint8_t *fps, uint8_t *loop);
esp_err_t stream_flash_get_slot_hash(int slot, uint8_t *out, size_t len);

esp_err_t stream_engine_init(ws2812_matrix_handle_t m);
bool stream_engine_push_frame(const uint8_t *chain_rgb, size_t len);
void stream_engine_set_brightness(uint8_t pct);
void stream_engine_stop(void);
bool stream_engine_is_running(void);

void stream_handle_frame_chunk(const uint8_t *data, size_t len);
void stream_handle_transfer_hash(const uint8_t *hash, size_t len);
void stream_handle_transfer_meta(const uint8_t *meta, size_t len);
void stream_handle_play_cmd(uint8_t cmd);
void stream_set_status(uint8_t status);

#ifdef __cplusplus
namespace stream {
constexpr uint32_t CLUSTER_ID = STREAM_CLUSTER_ID;
constexpr uint32_t ATTR_MATRIX_WIDTH = STREAM_ATTR_MATRIX_WIDTH;
constexpr uint32_t ATTR_MATRIX_HEIGHT = STREAM_ATTR_MATRIX_HEIGHT;
constexpr uint32_t ATTR_PIXEL_COUNT = STREAM_ATTR_PIXEL_COUNT;
constexpr uint32_t ATTR_SERPENTINE = STREAM_ATTR_SERPENTINE;
constexpr uint32_t ATTR_CACHED = STREAM_ATTR_CACHED;
constexpr uint32_t ATTR_TRANSFER_HASH = STREAM_ATTR_TRANSFER_HASH;
constexpr uint32_t ATTR_TRANSFER_META = STREAM_ATTR_TRANSFER_META;
constexpr uint32_t ATTR_FRAME_CHUNK = STREAM_ATTR_FRAME_CHUNK;
constexpr uint32_t ATTR_STATUS = STREAM_ATTR_STATUS;
constexpr uint32_t ATTR_PLAY_CMD = STREAM_ATTR_PLAY_CMD;
constexpr uint32_t ATTR_ACTIVE = STREAM_ATTR_ACTIVE;
esp_matter::cluster_t *cluster_create(esp_matter::endpoint_t *ep);
}
#endif
