#include "ws2812_matrix.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "ws2812_matrix"
#define LED_COUNT (CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT)

/* The strip handle is shared between the animation playback task and the Matter
 * thread (static color updates). led_strip refresh (rmt_enable/transmit/disable)
 * is not atomic, so every multi-step operation runs under this mutex. The same
 * mutex gates SPI flash IO (see stream_flash) against the refresh, because a
 * concurrent flash op starves the non-IRAM RMT ISR on ESP32 classic and corrupts
 * the WS2812 bitstream. */
static SemaphoreHandle_t s_mtx;

void ws2812_matrix_lock(void) {
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}
void ws2812_matrix_unlock(void) {
    if (s_mtx) xSemaphoreGive(s_mtx);
}

ws2812_matrix_handle_t ws2812_matrix_init(void) {
    led_strip_config_t cfg = { .strip_gpio_num = CONFIG_WS2812_GPIO, .max_leds = LED_COUNT };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    led_strip_handle_t strip = NULL;
    if (led_strip_new_rmt_device(&cfg, &rmt, &strip) != ESP_OK) return NULL;
    s_mtx = xSemaphoreCreateMutex();
    ws2812_matrix_lock();
    led_strip_clear(strip);
    ws2812_matrix_unlock();
    return (ws2812_matrix_handle_t)strip;
}

void ws2812_matrix_show_frame(ws2812_matrix_handle_t h, const uint8_t *rgb, size_t len) {
    led_strip_handle_t strip = (led_strip_handle_t)h;
    ws2812_matrix_lock();
    for (size_t i = 0; i < LED_COUNT && (i + 1) * 3 <= len; i++) {
        led_strip_set_pixel(strip, i, rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    }
    led_strip_refresh(strip);
    ws2812_matrix_unlock();
}

void ws2812_matrix_fill_rgb(ws2812_matrix_handle_t h, uint8_t r, uint8_t g, uint8_t b) {
    led_strip_handle_t strip = (led_strip_handle_t)h;
    ws2812_matrix_lock();
    for (size_t i = 0; i < LED_COUNT; i++) led_strip_set_pixel(strip, i, r, g, b);
    led_strip_refresh(strip);
    ws2812_matrix_unlock();
}

void ws2812_matrix_clear(ws2812_matrix_handle_t h) {
    led_strip_handle_t strip = (led_strip_handle_t)h;
    ws2812_matrix_lock();
    led_strip_clear(strip);
    ws2812_matrix_unlock();
}
