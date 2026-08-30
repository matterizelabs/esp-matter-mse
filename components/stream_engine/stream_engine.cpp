#include "stream_engine.h"
#include "esp_log.h"
#include "esp_matter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include <atomic>
#include <string.h>

#define TAG "stream_engine"
#define LED_COUNT (CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT)
#define FRAME_BYTES (LED_COUNT * 3)
#define RING_LEN 8
#define WORK_Q_LEN 16
#define CHUNK_POOL_LEN 8
#define CHUNK_BUF_SZ 1100
#define BITMAP_BYTES ((STREAM_MAX_FRAMES + 7) / 8)

enum {
    STREAM_STATUS_IDLE = 0,
    STREAM_STATUS_ANNOUNCED = 1,
    STREAM_STATUS_RECEIVING = 2,
    STREAM_STATUS_READY = 4,   /* cached stream found */
    STREAM_STATUS_PLAYING = 4,
    STREAM_STATUS_ERROR = 5,
};

typedef enum { WORK_START = 1, WORK_META, WORK_CHUNK, WORK_PLAY, WORK_STOP, WORK_CLEAR } work_type_t;

typedef struct {
    uint8_t type;
    uint16_t len;
    int16_t pool_idx;
    uint8_t payload[32];
} work_item_t;

/* ---- shared with chip thread ---- */
static ws2812_matrix_handle_t s_matrix;
static QueueHandle_t s_work_q;      /* work items, chip thread -> io task */
static QueueHandle_t s_fill_q;      /* ring slot indices with fresh frames, io -> playback */
static QueueHandle_t s_free_q;      /* ring slot indices available to write, playback -> io */
static QueueHandle_t s_pool_q;      /* chunk buffer indices available to copy into */
static SemaphoreHandle_t s_ring_mtx;   /* serializes ring push/pop/reset */
static SemaphoreHandle_t s_stop_done;  /* ack from io task that STOP was processed */
static uint8_t s_ring[RING_LEN][FRAME_BYTES];
static uint8_t s_pool[CHUNK_POOL_LEN][CHUNK_BUF_SZ];
static std::atomic<uint8_t> s_brightness{100};
static std::atomic<bool> s_running{false};

/* ---- transfer + playback state, owned by io task ---- */
static int s_slot = -1;
static bool s_cached_hit = false;
static bool s_transfer = false;
static uint8_t s_pending_hash[32];
static uint16_t s_total = 0;
static uint8_t s_fps = CONFIG_STREAM_FPS;
static uint8_t s_loop = 1;
static uint8_t s_bitmap[BITMAP_BYTES];
static int s_cache_slot = -1;
static uint16_t s_cache_idx = 0;
static uint16_t s_cache_frames = 0;
static uint8_t s_cache_fps = CONFIG_STREAM_FPS;
static uint8_t s_last_status = 0xFF;

extern uint16_t light_endpoint_id;

/* ------------------------------------------------------------------------- */
/* attribute reporting (io task context)                                      */
/* ------------------------------------------------------------------------- */

void stream_set_status(uint8_t status) {
    if (status == s_last_status) return;   /* only report on change, not per chunk */
    s_last_status = status;
    ESP_LOGI(TAG, "transfer status -> %u", status);
    esp_matter_attr_val_t val = esp_matter_enum8(status);
    if (esp_matter::attribute::update(light_endpoint_id, stream::CLUSTER_ID, stream::ATTR_STATUS, &val) != ESP_OK) {
        ESP_LOGE(TAG, "failed to update ATTR_STATUS");
    }
}

static void set_active(const uint8_t *hash) {
    esp_matter_attr_val_t val = esp_matter_octet_str((uint8_t *)hash, 32);
    if (esp_matter::attribute::update(light_endpoint_id, stream::CLUSTER_ID, stream::ATTR_ACTIVE, &val) != ESP_OK) {
        ESP_LOGE(TAG, "failed to update ATTR_ACTIVE");
    }
}

static void report_cached(void) {
    static uint8_t blobs[STREAM_SLOT_COUNT * 32];
    memset(blobs, 0, sizeof(blobs));
    for (int i = 0; i < STREAM_SLOT_COUNT; i++) stream_flash_get_slot_hash(i, blobs + i * 32, 32);
    esp_matter_attr_val_t val = esp_matter_octet_str(blobs, sizeof(blobs));
    if (esp_matter::attribute::update(light_endpoint_id, stream::CLUSTER_ID, stream::ATTR_CACHED, &val) != ESP_OK) {
        ESP_LOGE(TAG, "failed to update ATTR_CACHED");
    }
}

/* ------------------------------------------------------------------------- */
/* ring buffer: io task is the single producer, playback task the consumer.   */
/* A slot is owned by exactly one side at a time: free_q -> (write) ->        */
/* fill_q -> (copy) -> free_q. The consumer copies and releases the slot      */
/* inside the same critical section, so a slot can never be rewritten while   */
/* the consumer still reads it (no torn frames).                              */
/* ------------------------------------------------------------------------- */

static bool ring_push(const uint8_t *frame) {
    uint8_t idx;
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    bool ok = false;
    if (xQueueReceive(s_free_q, &idx, 0) == pdTRUE) {
        memcpy(s_ring[idx], frame, FRAME_BYTES);
        xQueueSend(s_fill_q, &idx, 0);
        ok = true;
    }
    xSemaphoreGive(s_ring_mtx);
    return ok;
}

static void ring_reset(void) {
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    xQueueReset(s_fill_q);
    xQueueReset(s_free_q);
    for (uint8_t i = 0; i < RING_LEN; i++) xQueueSend(s_free_q, &i, 0);
    xSemaphoreGive(s_ring_mtx);
}

bool stream_engine_push_frame(const uint8_t *chain_rgb, size_t len) {
    if (len != FRAME_BYTES) return false;
    return ring_push(chain_rgb);
}

static void playback_task(void *arg) {
    static uint8_t scratch[FRAME_BYTES];
    uint8_t idx;
    for (;;) {
        if (xQueueReceive(s_fill_q, &idx, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
        uint8_t b = s_brightness.load(std::memory_order_relaxed);
        for (int i = 0; i < FRAME_BYTES; i++) scratch[i] = (uint8_t)(s_ring[idx][i] * b / 100);
        xQueueSend(s_free_q, &idx, 0);
        xSemaphoreGive(s_ring_mtx);
        ws2812_matrix_show_frame(s_matrix, scratch, FRAME_BYTES);
    }
}

/* ------------------------------------------------------------------------- */
/* transfer state machine (io task)                                           */
/*   TransferHash (announce) -> TransferMeta -> FrameChunk* -> PlayCmd        */
/* ------------------------------------------------------------------------- */

static bool bitmap_get(uint32_t fi) { return (s_bitmap[fi >> 3] & (1u << (fi & 7))) != 0; }
static void bitmap_set(uint32_t fi) { s_bitmap[fi >> 3] |= (1u << (fi & 7)); }

static void fail_transfer(void) {
    if (s_transfer && s_slot >= 0) stream_flash_abort(s_slot);
    s_transfer = false;
    s_cached_hit = false;
    s_slot = -1;
    s_total = 0;
    report_cached();
    stream_set_status(STREAM_STATUS_ERROR);
}

static void stop_playback(void) {
    s_running = false;
    s_cache_slot = -1;
    s_cache_idx = 0;
    ring_reset();   /* drop frames still queued so nothing repaints after the clear */
    ws2812_matrix_clear(s_matrix);
    uint8_t zero[32] = {0};
    set_active(zero);
}

static void handle_start(const uint8_t *hash) {
    memcpy(s_pending_hash, hash, 32);
    int found = stream_flash_find(hash);
    if (found >= 0) {
        /* already cached: playback from flash, ignore any re-streamed chunks */
        s_slot = found;
        s_cached_hit = true;
        s_transfer = false;
        stream_set_status(STREAM_STATUS_READY);
        return;
    }
    /* miss: stop current playback first so its slot can be reclaimed */
    stop_playback();
    s_cached_hit = false;
    s_slot = stream_flash_alloc_slot();
    if (s_slot < 0) {
        stream_set_status(STREAM_STATUS_ERROR);
        return;
    }
    s_transfer = true;
    s_total = 0;
    s_fps = CONFIG_STREAM_FPS;
    s_loop = 1;
    memset(s_bitmap, 0, sizeof(s_bitmap));
    report_cached();
    stream_set_status(STREAM_STATUS_ANNOUNCED);
}

static void handle_meta(const uint8_t *meta, size_t len) {
    if (len != 6) { fail_transfer(); return; }
    uint16_t total = (uint16_t)(meta[0] | (meta[1] << 8));
    uint8_t fps = meta[2];
    uint8_t loop = meta[3];
    uint8_t width = meta[4];
    uint8_t height = meta[5];
    if (width != CONFIG_MATRIX_WIDTH || height != CONFIG_MATRIX_HEIGHT) { fail_transfer(); return; }
    if (total == 0 || total > STREAM_MAX_FRAMES) { fail_transfer(); return; }
    if ((uint32_t)total * FRAME_BYTES > STREAM_SLOT_SIZE) { fail_transfer(); return; }
    s_total = total;
    s_fps = (fps >= 1 && fps <= 60) ? fps : CONFIG_STREAM_FPS;
    s_loop = loop ? 1 : 0;
    ESP_LOGI(TAG, "transfer meta: %u frames @ %u fps, loop=%u, %ux%u", total, s_fps, s_loop, width, height);
}

static void handle_chunk(const uint8_t *data, size_t len) {
    stream_chunk_hdr_t hdr;
    if (!stream_parse_chunk_header(data, len, &hdr)) return;
    if (hdr.width != CONFIG_MATRIX_WIDTH || hdr.height != CONFIG_MATRIX_HEIGHT) { fail_transfer(); return; }
    if (len < 6 + (size_t)hdr.count * FRAME_BYTES) { fail_transfer(); return; }
    if (!s_transfer) return;   /* cached-hit: streamed chunks are ignored, nothing is rewritten */
    const uint8_t *px = data + 6;
    for (int k = 0; k < hdr.count; k++) {
        uint32_t fi = (uint32_t)hdr.frame_index + k;
        if (fi >= s_total) { fail_transfer(); return; }
        if (bitmap_get(fi)) continue;   /* retransmitted frame: skip write/hash/push */
        bitmap_set(fi);
        const uint8_t *frame = px + k * FRAME_BYTES;
        stream_engine_push_frame(frame, FRAME_BYTES);   /* preview immediately (drop if ring full) */
        if (stream_flash_write(s_slot, fi * FRAME_BYTES, frame, FRAME_BYTES) != ESP_OK) {
            fail_transfer();
            return;
        }
    }
    stream_set_status(STREAM_STATUS_RECEIVING);   /* deduped: reported only on first chunk */
}

/* hash of the frames as persisted in flash must equal the announced hash;
 * works for out-of-order and duplicated frames, and catches torn slots */
static bool verify_slot(int slot, uint16_t total, const uint8_t *expect) {
    static uint8_t fbuf[FRAME_BYTES];
    uint8_t digest[32];
    size_t dlen = 0;
    psa_crypto_init();
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
    for (uint16_t i = 0; i < total; i++) {
        if (stream_flash_read_frame(slot, i, fbuf, sizeof(fbuf)) != ESP_OK ||
            psa_hash_update(&op, fbuf, FRAME_BYTES) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            return false;
        }
    }
    psa_hash_finish(&op, digest, sizeof(digest), &dlen);
    return dlen == 32 && memcmp(digest, expect, 32) == 0;
}

static void handle_play(void) {
    if (s_cached_hit) {
        uint16_t frames = 0;
        uint8_t fps = 0, loop = 0;
        if (s_slot < 0 || stream_flash_get_slot_info(s_slot, &frames, &fps, &loop) != ESP_OK || frames == 0) {
            stream_set_status(STREAM_STATUS_ERROR);
            return;
        }
        /* verify persisted data still matches the slot hash; catches slots torn
         * by a power loss during an earlier interrupted transfer */
        if (!verify_slot(s_slot, frames, s_pending_hash)) {
            stream_set_status(STREAM_STATUS_ERROR);
            return;
        }
        ring_reset();
        s_cache_slot = s_slot;
        s_cache_frames = frames;
        s_cache_fps = (fps >= 1 && fps <= 60) ? fps : CONFIG_STREAM_FPS;
        s_cache_idx = 0;
        s_running = true;
        set_active(s_pending_hash);
        stream_set_status(STREAM_STATUS_PLAYING);
        return;
    }
    if (!s_transfer || s_slot < 0 || s_total == 0) { fail_transfer(); return; }
    if (!verify_slot(s_slot, s_total, s_pending_hash)) { fail_transfer(); return; }
    if (stream_flash_commit(s_slot, s_pending_hash, s_total, s_fps, s_loop) != ESP_OK) { fail_transfer(); return; }
    s_transfer = false;
    s_cached_hit = false;
    ring_reset();
    s_cache_slot = s_slot;
    s_cache_frames = s_total;
    s_cache_fps = s_fps;
    s_cache_idx = 0;
    s_running = true;
    set_active(s_pending_hash);
    report_cached();
    stream_set_status(STREAM_STATUS_PLAYING);
}

static void handle_stop(void) {
    stop_playback();
    stream_set_status(STREAM_STATUS_IDLE);
    xSemaphoreGive(s_stop_done);
}

static void handle_clear(void) {
    stop_playback();
    stream_flash_clear_all();
    report_cached();
    stream_set_status(STREAM_STATUS_IDLE);
}

static void push_cache_frame(void) {
    if (!s_running || s_cache_slot < 0) return;
    static uint8_t frame[FRAME_BYTES];
    if (stream_flash_read_frame(s_cache_slot, s_cache_idx, frame, sizeof(frame)) != ESP_OK) {
        stop_playback();
        stream_set_status(STREAM_STATUS_ERROR);
        return;
    }
    stream_engine_push_frame(frame, FRAME_BYTES);
    s_cache_idx = (s_cache_idx + 1) % s_cache_frames;
}

/* sole producer of ring frames and sole flash user; keeps all flash latency
 * (sector erase, commit) off the Matter thread */
static void io_task(void *arg) {
    psa_crypto_init();
    work_item_t item;
    for (;;) {
        TickType_t wait = (s_running && s_cache_slot >= 0) ? pdMS_TO_TICKS(1000 / s_cache_fps) : portMAX_DELAY;
        if (xQueueReceive(s_work_q, &item, wait) == pdTRUE) {
            switch (item.type) {
            case WORK_START:
                handle_start(item.payload);
                break;
            case WORK_META:
                handle_meta(item.payload, item.len);
                break;
            case WORK_CHUNK: {
                uint8_t pidx = (uint8_t)item.pool_idx;
                if (pidx < CHUNK_POOL_LEN) {
                    handle_chunk(s_pool[pidx], item.len);
                    xQueueSend(s_pool_q, &pidx, 0);
                }
                break;
            }
            case WORK_PLAY:
                handle_play();
                break;
            case WORK_STOP:
                handle_stop();
                break;
            case WORK_CLEAR:
                handle_clear();
                break;
            default:
                break;
            }
        } else {
            push_cache_frame();
        }
    }
}

/* ------------------------------------------------------------------------- */
/* chip-thread entry points: validate and queue, never touch flash or ring    */
/* ------------------------------------------------------------------------- */

static bool queue_item(uint8_t type, const void *payload, size_t len) {
    work_item_t it;
    memset(&it, 0, sizeof(it));
    it.type = type;
    it.len = len;
    if (payload && len) memcpy(it.payload, payload, len);
    return xQueueSend(s_work_q, &it, pdMS_TO_TICKS(50)) == pdTRUE;
}

void stream_handle_transfer_hash(const uint8_t *hash, size_t len) {
    if (len != 32) return;
    queue_item(WORK_START, hash, 32);
}

void stream_handle_transfer_meta(const uint8_t *meta, size_t len) {
    if (len != 6) return;
    queue_item(WORK_META, meta, len);
}

void stream_handle_frame_chunk(const uint8_t *data, size_t len) {
    stream_chunk_hdr_t hdr;
    if (!stream_parse_chunk_header(data, len, &hdr)) return;
    if (hdr.width != CONFIG_MATRIX_WIDTH || hdr.height != CONFIG_MATRIX_HEIGHT) return;
    if (len < 6 + (size_t)hdr.count * FRAME_BYTES || len > CHUNK_BUF_SZ) return;
    uint8_t pidx;
    if (xQueueReceive(s_pool_q, &pidx, 0) != pdTRUE) {
        ESP_LOGW(TAG, "chunk pool exhausted, dropping chunk");
        return;
    }
    memcpy(s_pool[pidx], data, len);
    work_item_t it;
    memset(&it, 0, sizeof(it));
    it.type = WORK_CHUNK;
    it.len = len;
    it.pool_idx = pidx;
    if (xQueueSend(s_work_q, &it, pdMS_TO_TICKS(50)) != pdTRUE) {
        xQueueSend(s_pool_q, &pidx, 0);
    }
}

void stream_handle_play_cmd(uint8_t cmd) {
    if (cmd == 1) {
        queue_item(WORK_PLAY, NULL, 0);
    } else if (cmd == 2) {
        queue_item(WORK_STOP, NULL, 0);
    } else if (cmd == 3) {
        queue_item(WORK_CLEAR, NULL, 0);
    }
}

/* ------------------------------------------------------------------------- */
/* public control                                                             */
/* ------------------------------------------------------------------------- */

esp_err_t stream_engine_init(ws2812_matrix_handle_t m) {
    s_matrix = m;
    s_work_q = xQueueCreate(WORK_Q_LEN, sizeof(work_item_t));
    s_fill_q = xQueueCreate(RING_LEN, sizeof(uint8_t));
    s_free_q = xQueueCreate(RING_LEN, sizeof(uint8_t));
    s_pool_q = xQueueCreate(CHUNK_POOL_LEN, sizeof(uint8_t));
    s_ring_mtx = xSemaphoreCreateMutex();
    s_stop_done = xSemaphoreCreateBinary();
    uint8_t i;
    for (i = 0; i < RING_LEN; i++) xQueueSend(s_free_q, &i, 0);
    for (i = 0; i < CHUNK_POOL_LEN; i++) xQueueSend(s_pool_q, &i, 0);
    xTaskCreate(playback_task, "stream_play", 4096, NULL, 10, NULL);
    xTaskCreate(io_task, "stream_io", 8192, NULL, 9, NULL);
    return ESP_OK;
}

void stream_engine_set_brightness(uint8_t pct) { s_brightness.store(pct > 100 ? 100 : pct, std::memory_order_relaxed); }

void stream_engine_stop(void) {
    while (xSemaphoreTake(s_stop_done, 0) == pdTRUE) { /* drain stale acks */
    }
    work_item_t it;
    memset(&it, 0, sizeof(it));
    it.type = WORK_STOP;
    if (xQueueSend(s_work_q, &it, pdMS_TO_TICKS(100)) != pdTRUE) return;
    xSemaphoreTake(s_stop_done, pdMS_TO_TICKS(1000));
}

bool stream_engine_is_running(void) { return s_running.load(std::memory_order_relaxed); }
