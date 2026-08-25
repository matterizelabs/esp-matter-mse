// firmware/components/ws2812_board/device.c
#include <esp_log.h>
#include <led_driver.h>
#include <button_gpio.h>
#include <driver/gpio.h>

led_driver_config_t led_driver_get_config() {
    led_driver_config_t config = { .gpio = CONFIG_WS2812_GPIO, .channel = 0 };
    return config;
}
button_gpio_config_t button_driver_get_config() {
    button_gpio_config_t config = { .gpio_num = GPIO_NUM_0, .active_level = 0 };
    return config;
}
