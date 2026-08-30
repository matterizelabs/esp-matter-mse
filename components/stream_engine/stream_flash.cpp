#include "stream_engine.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "ws2812_matrix.h"
#include <string.h>

#define TAG "stream_flash"
#define FRAME_BYTES (CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3)
#define META_SIZE 4096
#define DATA_OFFSET META_SIZE
#define SECTOR_SIZE 4096
#define SLOT_SECTORS (STREAM_SLOT_SIZE / SECTOR_SIZE)
#define MAGIC 0x414E494D

typedef struct {
    uint32_t magic;
    uint32_t version;
    struct { uint8_t hash[32]; uint16_t total_frames; uint8_t fps, loop, state; uint32_t lru; } slots[STREAM_SLOT_COUNT];
} stream_flash_meta_t;

static const esp_partition_t *s_part;
static stream_flash_meta_t s_meta;
static uint32_t s_tick = 0;
/* per-slot bitmap of 4KB sectors erased this write session; unknown after boot, so a
 * slot's first write always erases its sectors before programming */
static uint32_t s_sector_erased[STREAM_SLOT_COUNT];
/* slots being written by an in-progress transfer: excluded from find()/alloc()/playback
 * so a half-overwritten slot can never be reported as cached */
static uint8_t s_busy[STREAM_SLOT_COUNT];

/* s_meta/s_busy/s_sector_erased are owned by the single stream io task. SPI flash
 * access is additionally gated by ws2812_matrix_lock so a flash op never overlaps the
 * blocking RMT refresh, which would starve the non-IRAM RMT ISR on ESP32 classic. */

static esp_err_t persist_meta(void) {
    esp_err_t err = esp_partition_erase_range(s_part, 0, META_SIZE);
    if (err != ESP_OK) return err;
    return esp_partition_write(s_part, 0, &s_meta, sizeof(s_meta));
}

esp_err_t stream_flash_init(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "stream_cache");
    if (!s_part) { ESP_LOGE(TAG, "stream_cache partition not found"); return ESP_ERR_NOT_FOUND; }
    if (esp_partition_read(s_part, 0, &s_meta, sizeof(s_meta)) != ESP_OK || s_meta.magic != MAGIC) {
        memset(&s_meta, 0, sizeof(s_meta));
        s_meta.magic = MAGIC;
        s_meta.version = 1;
        esp_err_t err = esp_partition_erase_range(s_part, 0, META_SIZE);
        if (err != ESP_OK) return err;
        err = esp_partition_write(s_part, 0, &s_meta, sizeof(s_meta));
        if (err != ESP_OK) return err;
    }
    for (int i = 0; i < STREAM_SLOT_COUNT; i++)
        if (s_meta.slots[i].lru > s_tick) s_tick = s_meta.slots[i].lru;
    memset(s_busy, 0, sizeof(s_busy));
    memset(s_sector_erased, 0, sizeof(s_sector_erased));
    return ESP_OK;
}

int stream_flash_find(const uint8_t hash[32]) {
    for (int i = 0; i < STREAM_SLOT_COUNT; i++)
        if (!s_busy[i] && s_meta.slots[i].state == 1 && memcmp(s_meta.slots[i].hash, hash, 32) == 0) return i;
    return -1;
}

int stream_flash_alloc_slot(void) {
    uint32_t min = UINT32_MAX;
    int pick = -1;
    for (int i = 0; i < STREAM_SLOT_COUNT; i++) {
        if (s_busy[i]) continue;
        if (s_meta.slots[i].lru < min) { min = s_meta.slots[i].lru; pick = i; }
    }
    if (pick < 0) return -1;
    s_busy[pick] = 1;
    s_sector_erased[pick] = 0;
    s_meta.slots[pick].state = 0; /* old content invalid from the moment the slot is claimed */
    return pick;
}

esp_err_t stream_flash_abort(int slot) {
    if (slot < 0 || slot >= STREAM_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    s_busy[slot] = 0;
    s_meta.slots[slot].state = 0;
    ws2812_matrix_lock();
    esp_err_t err = persist_meta();
    ws2812_matrix_unlock();
    return err;
}

esp_err_t stream_flash_clear_all(void) {
    for (int i = 0; i < STREAM_SLOT_COUNT; i++) {
        s_busy[i] = 0;
        s_meta.slots[i].state = 0;
    }
    ws2812_matrix_lock();
    esp_err_t err = persist_meta();
    ws2812_matrix_unlock();
    return err;
}

esp_err_t stream_flash_commit(int slot, const uint8_t hash[32], uint16_t total_frames, uint8_t fps, uint8_t loop) {
    if (slot < 0 || slot >= STREAM_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    memcpy(s_meta.slots[slot].hash, hash, 32);
    s_meta.slots[slot].total_frames = total_frames;
    s_meta.slots[slot].fps = fps;
    s_meta.slots[slot].loop = loop;
    s_meta.slots[slot].state = 1;
    s_meta.slots[slot].lru = ++s_tick;
    s_busy[slot] = 0;
    ws2812_matrix_lock();
    esp_err_t err = persist_meta();
    ws2812_matrix_unlock();
    return err;
}

esp_err_t stream_flash_write(int slot, uint32_t offset, const uint8_t *data, size_t len) {
    if (slot < 0 || slot >= STREAM_SLOT_COUNT || !data || len == 0) return ESP_ERR_INVALID_ARG;
    if ((uint64_t)offset + len > STREAM_SLOT_SIZE) return ESP_ERR_INVALID_ARG;
    uint32_t base = DATA_OFFSET + (uint32_t)slot * STREAM_SLOT_SIZE;
    uint32_t s0 = offset / SECTOR_SIZE;
    uint32_t s1 = (offset + len - 1) / SECTOR_SIZE;
    ws2812_matrix_lock();
    for (uint32_t s = s0; s <= s1; s++) {
        if (!(s_sector_erased[slot] & (1u << s))) {
            esp_err_t err = esp_partition_erase_range(s_part, base + s * SECTOR_SIZE, SECTOR_SIZE);
            if (err != ESP_OK) {
                ws2812_matrix_unlock();
                return err;
            }
            s_sector_erased[slot] |= (1u << s);
        }
    }
    esp_err_t err = esp_partition_write(s_part, base + offset, data, len);
    ws2812_matrix_unlock();
    return err;
}

esp_err_t stream_flash_read_frame(int slot, uint32_t frame_index, uint8_t *out, size_t out_len) {
    if (slot < 0 || slot >= STREAM_SLOT_COUNT || !out || out_len < FRAME_BYTES) return ESP_ERR_INVALID_SIZE;
    if ((uint64_t)(frame_index + 1) * FRAME_BYTES > STREAM_SLOT_SIZE) return ESP_ERR_INVALID_ARG;
    ws2812_matrix_lock();
    esp_err_t err = esp_partition_read(s_part, DATA_OFFSET + (uint32_t)slot * STREAM_SLOT_SIZE + frame_index * FRAME_BYTES, out,
                              FRAME_BYTES);
    ws2812_matrix_unlock();
    return err;
}

esp_err_t stream_flash_get_slot_info(int slot, uint16_t *total_frames, uint8_t *fps, uint8_t *loop) {
    if (slot < 0 || slot >= STREAM_SLOT_COUNT || s_busy[slot] || s_meta.slots[slot].state != 1) return ESP_ERR_INVALID_STATE;
    if (total_frames) *total_frames = s_meta.slots[slot].total_frames;
    if (fps) *fps = s_meta.slots[slot].fps;
    if (loop) *loop = s_meta.slots[slot].loop;
    return ESP_OK;
}

esp_err_t stream_flash_get_slot_hash(int slot, uint8_t *out, size_t len) {
    if (slot < 0 || slot >= STREAM_SLOT_COUNT || !out || len < 32 || s_busy[slot] || s_meta.slots[slot].state != 1)
        return ESP_ERR_INVALID_STATE;
    memcpy(out, s_meta.slots[slot].hash, 32);
    return ESP_OK;
}
