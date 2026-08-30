#pragma once
#include <stdint.h>
#include <stddef.h>
#include <button_gpio.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void *ws2812_matrix_handle_t;
ws2812_matrix_handle_t ws2812_matrix_init(void);
void ws2812_matrix_show_frame(ws2812_matrix_handle_t h, const uint8_t *rgb, size_t len);
void ws2812_matrix_fill_rgb(ws2812_matrix_handle_t h, uint8_t r, uint8_t g, uint8_t b);
void ws2812_matrix_clear(ws2812_matrix_handle_t h);
/* Serialize any SPI flash access against the RMT refresh. On ESP32 classic the RMT
 * TX ISR is not IRAM-flagged, so a concurrent flash op (erase/write/read) starves the
 * symbol FIFO and corrupts the WS2812 stream. Callers doing flash IO hold this lock;
 * the show/fill/clear paths hold it internally during the blocking refresh. */
void ws2812_matrix_lock(void);
void ws2812_matrix_unlock(void);
button_gpio_config_t button_driver_get_config(void);
#ifdef __cplusplus
}
#endif
