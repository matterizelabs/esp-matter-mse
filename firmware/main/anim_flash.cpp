#include "anim_flash.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>

#define TAG "anim_flash"
#define FRAME_BYTES (CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3)
#define META_SIZE 4096
#define DATA_OFFSET META_SIZE

typedef struct {
    uint32_t magic;
    uint32_t version;
    struct { uint8_t hash[32]; uint16_t total_frames; uint8_t fps, loop, state; uint32_t lru; } slots[ANIM_SLOT_COUNT];
} anim_meta_t;

static const esp_partition_t *s_part;
static anim_meta_t s_meta;
static uint32_t s_tick = 0;

esp_err_t anim_flash_init(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "anim_cache");
    if (!s_part) { ESP_LOGE(TAG, "anim_cache partition not found"); return ESP_ERR_NOT_FOUND; }
    if (esp_partition_read(s_part, 0, &s_meta, sizeof(s_meta)) != ESP_OK || s_meta.magic != 0x414E494D) {
        memset(&s_meta, 0, sizeof(s_meta)); s_meta.magic = 0x414E494D; s_meta.version = 1;
        esp_partition_erase_range(s_part, 0, s_part->size);
        esp_partition_write(s_part, 0, &s_meta, sizeof(s_meta));
    }
    return ESP_OK;
}
int anim_flash_find(const uint8_t hash[32]) {
    for (int i = 0; i < ANIM_SLOT_COUNT; i++)
        if (s_meta.slots[i].state == 1 && memcmp(s_meta.slots[i].hash, hash, 32) == 0) return i;
    return -1;
}
static int pick_lru_slot(void) {
    uint32_t min = UINT32_MAX; int pick = 0;
    for (int i = 0; i < ANIM_SLOT_COUNT; i++) if (s_meta.slots[i].lru < min) { min = s_meta.slots[i].lru; pick = i; }
    return pick;
}
int anim_flash_alloc_slot(void) { return pick_lru_slot(); }
esp_err_t anim_flash_write(int slot, const uint8_t *data, size_t len) {
    uint32_t off = DATA_OFFSET + (uint32_t)slot * ANIM_SLOT_SIZE;
    return esp_partition_write(s_part, off, data, len);
}
esp_err_t anim_flash_erase(int slot) {
    uint32_t off = DATA_OFFSET + (uint32_t)slot * ANIM_SLOT_SIZE;
    return esp_partition_erase_range(s_part, off, ANIM_SLOT_SIZE);
}
esp_err_t anim_flash_commit(int slot, const uint8_t hash[32], uint16_t total_frames, uint8_t fps, uint8_t loop) {
    memcpy(s_meta.slots[slot].hash, hash, 32);
    s_meta.slots[slot].total_frames = total_frames; s_meta.slots[slot].fps = fps;
    s_meta.slots[slot].loop = loop; s_meta.slots[slot].state = 1; s_meta.slots[slot].lru = ++s_tick;
    esp_partition_erase_range(s_part, 0, META_SIZE);
    return esp_partition_write(s_part, 0, &s_meta, sizeof(s_meta));
}
esp_err_t anim_flash_read_frame(int slot, uint32_t frame_index, uint8_t *out, size_t out_len) {
    uint32_t off = DATA_OFFSET + (uint32_t)slot * ANIM_SLOT_SIZE + frame_index * FRAME_BYTES;
    if (out_len < FRAME_BYTES) return ESP_ERR_INVALID_SIZE;
    return esp_partition_read(s_part, off, out, FRAME_BYTES);
}
