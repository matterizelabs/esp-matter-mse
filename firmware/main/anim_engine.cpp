#include "anim_engine.h"
#include "anim_codec.h"
#include "anim_flash.h"
#include "anim_cluster.h"
#include "esp_log.h"
#include "esp_matter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "psa/crypto.h"
#include <string.h>

#define TAG "anim_engine"
#define LED_COUNT (CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT)
#define FRAME_BYTES (LED_COUNT * 3)
#define RING_LEN 8
#define TICK_MS (1000 / CONFIG_ANIM_FPS)

static ws2812_matrix_handle_t s_matrix;
static QueueHandle_t s_q;          // queue of pointers into a static ring
static uint8_t s_ring[RING_LEN][FRAME_BYTES];
static uint32_t s_head = 0;        // next write slot
static volatile uint8_t s_brightness = 100;
static volatile bool s_running = false;

static void playback_task(void *arg) {
    while (true) {
        if (s_running) {
            void *slot = NULL;
            if (xQueueReceive(s_q, &slot, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
                uint8_t *frame = (uint8_t *)slot;
                // apply master brightness in-place (copy to scratch)
                static uint8_t scratch[FRAME_BYTES];
                for (int i = 0; i < FRAME_BYTES; i++) scratch[i] = frame[i] * s_brightness / 100;
                ws2812_matrix_show_frame(s_matrix, scratch, FRAME_BYTES);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        }
    }
}

esp_err_t anim_engine_init(ws2812_matrix_handle_t m) {
    s_matrix = m;
    s_q = xQueueCreate(RING_LEN, sizeof(void *));
    xTaskCreate(playback_task, "anim_play", 4096, NULL, 10, NULL);
    return ESP_OK;
}

bool anim_engine_push_frame(const uint8_t *chain_rgb, size_t len) {
    if (len != FRAME_BYTES) return false;
    if (uxQueueSpacesAvailable(s_q) == 0) return false;  // clean drop if full (no slot corruption)
    uint8_t *slot = s_ring[s_head % RING_LEN];
    memcpy(slot, chain_rgb, FRAME_BYTES);
    if (xQueueSend(s_q, &slot, 0) != pdTRUE) return false;
    s_head++;
    return true;
}
void anim_engine_set_brightness(uint8_t pct) { s_brightness = pct > 100 ? 100 : pct; }
void anim_engine_stop(void) { s_running = false; }
bool anim_engine_is_running(void) { return s_running; }

/* ------------------------------------------------------------------------- */
/* Transfer state machine:                                                    */
/*   TransferHash (announce) -> TransferMeta -> FrameChunk* -> PlayCmd        */
/* ------------------------------------------------------------------------- */

extern uint16_t light_endpoint_id;

static int s_slot = -1;
static uint32_t s_write_cursor = 0;
static uint16_t s_total = 0;
static uint8_t s_pending_hash[32];
static psa_hash_operation_t s_hash_op = PSA_HASH_OPERATION_INIT;
static bool s_announced = false;
static bool s_cached_hit = false;
static int s_cache_slot = -1;
static uint16_t s_cache_frames = 0;

static void cache_fill_task(void *arg) {
    uint32_t idx = 0;
    for (int i = 0; i < RING_LEN && s_running; i++) {   // prime the ring buffer
        uint8_t frame[FRAME_BYTES];
        if (anim_flash_read_frame(s_cache_slot, idx, frame, FRAME_BYTES) != ESP_OK) break;
        anim_engine_push_frame(frame, FRAME_BYTES);
        idx = (idx + 1) % s_cache_frames;
    }
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        uint8_t frame[FRAME_BYTES];
        if (anim_flash_read_frame(s_cache_slot, idx, frame, FRAME_BYTES) == ESP_OK)
            anim_engine_push_frame(frame, FRAME_BYTES);
        idx = (idx + 1) % s_cache_frames;
    }
    vTaskDelete(NULL);
}

void anim_set_status(uint8_t status) {
    ESP_LOGI(TAG, "transfer status -> %u", status);
    esp_matter_attr_val_t val = esp_matter_enum8(status);
    esp_err_t err = esp_matter::attribute::update(light_endpoint_id, anim::CLUSTER_ID, anim::ATTR_STATUS, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to update ATTR_STATUS: %d", err);
    }
}

void anim_handle_transfer_hash(const uint8_t *hash, size_t len) {
    if (len != 32) return;
    memcpy(s_pending_hash, hash, 32);
    s_slot = anim_flash_find(hash);
    s_cached_hit = (s_slot >= 0);
    anim_set_status(s_slot >= 0 ? 4 /*READY*/ : 1 /*ANNOUNCED*/);
    if (s_slot < 0) {
        s_slot = anim_flash_alloc_slot();
        anim_flash_erase(s_slot);
        s_write_cursor = 0;
    }
    psa_crypto_init();
    psa_hash_abort(&s_hash_op); /* safe no-op when already inactive */
    psa_hash_setup(&s_hash_op, PSA_ALG_SHA_256);
    s_announced = true;
}

void anim_handle_transfer_meta(const uint8_t *meta, size_t len) {
    if (len != 6) { anim_set_status(5 /*ERROR*/); return; }
    uint16_t total_frames = (uint16_t)(meta[0] | (meta[1] << 8));
    uint8_t fps = meta[2];
    uint8_t loop = meta[3];
    uint8_t width = meta[4];
    uint8_t height = meta[5];
    if (width != CONFIG_MATRIX_WIDTH || height != CONFIG_MATRIX_HEIGHT) { anim_set_status(5 /*ERROR*/); return; }
    s_total = total_frames;
    ESP_LOGI(TAG, "transfer meta: %u frames @ %u fps, loop=%u, %ux%u", total_frames, fps, loop, width, height);
}

void anim_handle_frame_chunk(const uint8_t *data, size_t len) {
    anim_chunk_hdr_t hdr;
    if (!anim_parse_chunk_header(data, len, &hdr)) return;
    if (hdr.width != CONFIG_MATRIX_WIDTH || hdr.height != CONFIG_MATRIX_HEIGHT) { anim_set_status(5 /*ERROR*/); return; }
    size_t fsz = hdr.width * hdr.height * 3;
    if (len < 6 + (size_t)hdr.count * fsz) { anim_set_status(5 /*ERROR*/); return; }
    const uint8_t *px = data + 6;
    for (int k = 0; k < hdr.count; k++) {
        const uint8_t *frame = px + k * fsz;
        anim_engine_push_frame(frame, fsz);          /* play immediately */
        if (s_slot >= 0) {                            /* persist in background */
            anim_flash_write(s_slot, (uint32_t)(hdr.frame_index + k) * fsz, frame, fsz);
            s_write_cursor += fsz;
        }
        psa_hash_update(&s_hash_op, frame, fsz);
    }
    anim_set_status(2 /*RECEIVING*/);
}

void anim_handle_play_cmd(uint8_t cmd) {
    if (cmd == 1) { /* PLAY */
        if (s_cached_hit) { /* hash already in flash: play from cache, no re-stream */
            uint8_t fps = CONFIG_ANIM_FPS;
            uint8_t loop = 1;
            if (anim_flash_get_slot_info(s_slot, &s_cache_frames, &fps, &loop) != ESP_OK || s_cache_frames == 0) {
                anim_set_status(5 /*ERROR*/);
                return;
            }
            xQueueReset(s_q);                    // discard the stale streamed frames already in the ring
            s_cache_slot = s_slot;
            s_running = true;
            xTaskCreate(cache_fill_task, "anim_cache_fill", 4096, NULL, 9, NULL);
            anim_set_status(4 /*PLAYING*/);
            return;
        }
        /* miss: verify streamed hash, commit slot, start loop */
        if (!s_announced) { anim_set_status(5 /*ERROR*/); return; }
        uint8_t digest[32] = {0};
        size_t digest_len = 0;
        if (psa_hash_finish(&s_hash_op, digest, sizeof(digest), &digest_len) != PSA_SUCCESS || digest_len != 32) {
            s_announced = false;
            anim_set_status(5 /*ERROR*/);
            return;
        }
        if (memcmp(digest, s_pending_hash, 32) != 0) {
            s_announced = false;
            anim_set_status(5 /*ERROR*/);
            return;
        }
        if (s_slot >= 0) anim_flash_commit(s_slot, s_pending_hash, s_total, CONFIG_ANIM_FPS, 1);
        s_announced = false;
        xQueueReset(s_q);                    // discard the stale streamed frames already in the ring
        s_cache_slot = s_slot;
        s_cache_frames = s_total;
        s_running = true;
        xTaskCreate(cache_fill_task, "anim_cache_fill", 4096, NULL, 9, NULL);
        anim_set_status(4 /*PLAYING*/);
    } else if (cmd == 2) { /* STOP */
        s_running = false;
        anim_set_status(0 /*IDLE*/);
    }
}
