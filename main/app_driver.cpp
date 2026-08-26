/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

#include <esp_matter.h>
#include <app_priv.h>
#include <common_macros.h>

#include <device.h>
#include <button_gpio.h>

#include <color_format.h>
#include "ws2812_matrix.h"
#include "anim_engine.h"

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t light_endpoint_id;

/* Current static color state (mirrors the stock led_driver's internal state). */
static bool s_power = false;
static uint8_t s_brightness = 0;    /* 0-100 (STANDARD_BRIGHTNESS) */
static HS_color_t s_hs = {0, 0};    /* hue 0-360, saturation 0-100 */
static uint32_t s_temp = 0;         /* Kelvin */
static XY_color_t s_xy = {0, 0};    /* Matter xy coordinates (0-65535) */
static uint8_t s_color_mode = (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation;

/* Compute the RGB for the current color mode + master brightness, then fill the whole matrix. */
static esp_err_t app_driver_update_static_color(ws2812_matrix_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t brightness = s_power ? s_brightness : 0;
    RGB_color_t rgb = {0, 0, 0};
    switch (s_color_mode) {
    case (uint8_t)ColorControl::ColorMode::kColorTemperature:
        temp_to_hs(s_temp, &s_hs);
        hsv_to_rgb(s_hs, brightness, &rgb);
        break;
    case (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY:
        /* xy_to_rgb expects brightness 0-255, hsv_to_rgb expects 0-100. */
        xy_to_rgb(s_xy, (uint8_t)(brightness * 255 / 100), &rgb);
        break;
    case (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation:
    default:
        hsv_to_rgb(s_hs, brightness, &rgb);
        break;
    }
    ws2812_matrix_fill_rgb(handle, rgb.red, rgb.green, rgb.blue);
    return ESP_OK;
}

static esp_err_t app_driver_light_set_power(ws2812_matrix_handle_t handle, esp_matter_attr_val_t *val)
{
    s_power = val->val.b;
    if (!s_power) {
        anim_engine_stop();
        ws2812_matrix_clear(handle);
        return ESP_OK;
    }
    return app_driver_update_static_color(handle);
}

static esp_err_t app_driver_light_set_brightness(ws2812_matrix_handle_t handle, esp_matter_attr_val_t *val)
{
    s_brightness = REMAP_TO_RANGE(val->val.u8, MATTER_BRIGHTNESS, STANDARD_BRIGHTNESS);
    anim_engine_set_brightness(REMAP_TO_RANGE(val->val.u8, MATTER_BRIGHTNESS, 100));
    /* Dim a running animation live; re-apply the static fill only in static mode. */
    if (s_power && !anim_engine_is_running()) {
        return app_driver_update_static_color(handle);
    }
    return ESP_OK;
}

static esp_err_t app_driver_light_set_hue(ws2812_matrix_handle_t handle, esp_matter_attr_val_t *val)
{
    anim_engine_stop();
    s_hs.hue = REMAP_TO_RANGE(val->val.u8, MATTER_HUE, STANDARD_HUE);
    s_color_mode = (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation;
    return app_driver_update_static_color(handle);
}

static esp_err_t app_driver_light_set_saturation(ws2812_matrix_handle_t handle, esp_matter_attr_val_t *val)
{
    anim_engine_stop();
    s_hs.saturation = REMAP_TO_RANGE(val->val.u8, MATTER_SATURATION, STANDARD_SATURATION);
    s_color_mode = (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation;
    return app_driver_update_static_color(handle);
}

static esp_err_t app_driver_light_set_temperature(ws2812_matrix_handle_t handle, esp_matter_attr_val_t *val)
{
    anim_engine_stop();
    s_temp = REMAP_TO_RANGE_INVERSE(val->val.u16, STANDARD_TEMPERATURE_FACTOR);
    s_color_mode = (uint8_t)ColorControl::ColorMode::kColorTemperature;
    return app_driver_update_static_color(handle);
}

static esp_err_t app_driver_light_set_xy(ws2812_matrix_handle_t handle, uint16_t x, uint16_t y)
{
    anim_engine_stop();
    s_xy.x = x;
    s_xy.y = y;
    s_color_mode = (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY;
    return app_driver_update_static_color(handle);
}

static void app_driver_button_toggle_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Toggle button pressed");
    uint16_t endpoint_id = light_endpoint_id;
    uint32_t cluster_id = OnOff::Id;
    uint32_t attribute_id = OnOff::Attributes::OnOff::Id;

    attribute_t *attribute = attribute::get(endpoint_id, cluster_id, attribute_id);

    esp_matter_attr_val_t val;
    attribute::get_val(attribute, &val);
    val.val.b = !val.val.b;
    attribute::update(endpoint_id, cluster_id, attribute_id, &val);
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    esp_err_t err = ESP_OK;
    if (endpoint_id == light_endpoint_id) {
        ws2812_matrix_handle_t handle = (ws2812_matrix_handle_t)driver_handle;
        if (cluster_id == OnOff::Id) {
            if (attribute_id == OnOff::Attributes::OnOff::Id) {
                err = app_driver_light_set_power(handle, val);
            }
        } else if (cluster_id == LevelControl::Id) {
            if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
                err = app_driver_light_set_brightness(handle, val);
            }
        } else if (cluster_id == ColorControl::Id) {
            if (attribute_id == ColorControl::Attributes::CurrentHue::Id) {
                err = app_driver_light_set_hue(handle, val);
            } else if (attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
                err = app_driver_light_set_saturation(handle, val);
            } else if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
                err = app_driver_light_set_temperature(handle, val);
            } else if (attribute_id == ColorControl::Attributes::CurrentX::Id) {
                err = app_driver_light_set_xy(handle, val->val.u16, s_xy.y);
            } else if (attribute_id == ColorControl::Attributes::CurrentY::Id) {
                err = app_driver_light_set_xy(handle, s_xy.x, val->val.u16);
            }
        }
    }
    return err;
}

esp_err_t app_driver_light_set_defaults(uint16_t endpoint_id)
{
    esp_err_t err = ESP_OK;
    void *priv_data = endpoint::get_priv_data(endpoint_id);
    ws2812_matrix_handle_t handle = (ws2812_matrix_handle_t)priv_data;
    esp_matter_attr_val_t val;

    /* Setting brightness */
    attribute_t *attribute = attribute::get(endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    attribute::get_val(attribute, &val);
    s_brightness = REMAP_TO_RANGE(val.val.u8, MATTER_BRIGHTNESS, STANDARD_BRIGHTNESS);
    anim_engine_set_brightness(REMAP_TO_RANGE(val.val.u8, MATTER_BRIGHTNESS, 100));

    /* Setting color */
    attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorMode::Id);
    attribute::get_val(attribute, &val);
    s_color_mode = val.val.u8;
    if (val.val.u8 == (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation) {
        /* Setting hue */
        attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentHue::Id);
        attribute::get_val(attribute, &val);
        s_hs.hue = REMAP_TO_RANGE(val.val.u8, MATTER_HUE, STANDARD_HUE);
        /* Setting saturation */
        attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id);
        attribute::get_val(attribute, &val);
        s_hs.saturation = REMAP_TO_RANGE(val.val.u8, MATTER_SATURATION, STANDARD_SATURATION);
    } else if (val.val.u8 == (uint8_t)ColorControl::ColorMode::kColorTemperature) {
        /* Setting temperature */
        attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id);
        attribute::get_val(attribute, &val);
        s_temp = REMAP_TO_RANGE_INVERSE(val.val.u16, STANDARD_TEMPERATURE_FACTOR);
    } else if (val.val.u8 == (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY) {
        /* Setting XY coordinates */
        attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentX::Id);
        attribute::get_val(attribute, &val);
        s_xy.x = val.val.u16;
        attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentY::Id);
        attribute::get_val(attribute, &val);
        s_xy.y = val.val.u16;
    } else {
        ESP_LOGE(TAG, "Color mode not supported");
    }

    /* Setting power */
    attribute = attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    attribute::get_val(attribute, &val);
    s_power = val.val.b;

    err = app_driver_update_static_color(handle);

    return err;
}

app_driver_handle_t app_driver_light_init()
{
    /* Initialize the matrix driver (sole RMT owner). The stock led_driver is NOT used. */
    ws2812_matrix_handle_t handle = ws2812_matrix_init();
    if (handle) {
        anim_engine_init(handle);
    }
    return (app_driver_handle_t)handle;
}

app_driver_handle_t app_driver_button_init()
{
    /* Initialize button */
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = button_driver_get_config();

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return NULL;
    }

    iot_button_register_cb(handle, BUTTON_PRESS_DOWN, NULL, app_driver_button_toggle_cb, NULL);
    return (app_driver_handle_t)handle;
}
