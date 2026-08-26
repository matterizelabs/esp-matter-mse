#include <button_gpio.h>
#include <driver/gpio.h>

button_gpio_config_t button_driver_get_config() {
    button_gpio_config_t config = { .gpio_num = GPIO_NUM_0, .active_level = 0 };
    return config;
}
