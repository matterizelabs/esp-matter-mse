#include "anim_engine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define TAG "anim_engine"
#define LED_COUNT (CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT)
#define FRAME_BYTES (LED_COUNT * 3)
#define RING_LEN 8
#define TICK_MS (1000 / CONFIG_ANIM_FPS)

static ws2812_matrix_handle_t s_matrix;
static QueueHandle_t s_q;          // queue of pointers into a static ring
static uint8_t s_ring[RING_LEN][FRAME_BYTES];
static int s_head = 0;             // next write slot
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
    uint8_t *slot = s_ring[s_head % RING_LEN];
    memcpy(slot, chain_rgb, FRAME_BYTES);
    if (xQueueSend(s_q, &slot, 0) != pdTRUE) return false;  // drop if full (overrun)
    s_head++;
    return true;
}
void anim_engine_set_brightness(uint8_t pct) { s_brightness = pct > 100 ? 100 : pct; }
void anim_engine_stop(void) { s_running = false; }
