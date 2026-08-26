cmake_minimum_required(VERSION 3.5)
if (NOT ("${IDF_TARGET}" STREQUAL "esp32"))
    message(FATAL_ERROR "please set esp32 as the IDF_TARGET")
endif()
SET(device_type "")
SET(button_type iot)
SET(extra_components_dirs_append
    "$ENV{ESP_MATTER_PATH}/device_hal/button_driver/iot_button")
