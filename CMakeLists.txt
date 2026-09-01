# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Please set ESP_MATTER_PATH to the path of esp-matter repo")
endif(NOT DEFINED ENV{ESP_MATTER_PATH})

# Board profile is part of this repo; set the env var the SDK's build expects.
set(ENV{ESP_MATTER_DEVICE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/components/ws2812_matrix")

if(NOT DEFINED CLI_PROJECT_VER)
    set(PROJECT_VER "1.0")
else()
    set(PROJECT_VER "${CLI_PROJECT_VER}")
endif()

# Derive the numeric Matter SoftwareVersion (major*65536 + minor*256 + patch)
# from the semver string so the string and number cannot drift.
string(REPLACE "." ";" _PROJECT_VER_PARTS "${PROJECT_VER}")
list(GET _PROJECT_VER_PARTS 0 _PROJECT_VER_MAJOR)
list(GET _PROJECT_VER_PARTS 1 _PROJECT_VER_MINOR)
list(LENGTH _PROJECT_VER_PARTS _PROJECT_VER_PART_COUNT)
if(_PROJECT_VER_PART_COUNT GREATER 2)
    list(GET _PROJECT_VER_PARTS 2 _PROJECT_VER_PATCH)
else()
    set(_PROJECT_VER_PATCH 0)
endif()
math(EXPR PROJECT_VER_NUMBER "${_PROJECT_VER_MAJOR} * 65536 + ${_PROJECT_VER_MINOR} * 256 + ${_PROJECT_VER_PATCH}")

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# This should be done before using the IDF_TARGET variable.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)

set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    ${extra_components_dirs_append})

project(matter_mse)

# WARNING: This is just an example for using key for decrypting the encrypted OTA image
# Please do not use it as is.
if(CONFIG_ENABLE_ENCRYPTED_OTA)
    target_add_binary_data(light.elf "esp_image_encryption_key.pem" TEXT)
endif()

if(CONFIG_IDF_TARGET_ESP32C2)
    include(relinker)
endif()

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
# For RISCV chips, project_include.cmake sets -Wno-format, but does not clear various
# flags that depend on -Wformat
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "PROJECT_VER=\"${PROJECT_VER}\";PROJECT_VER_NUMBER=${PROJECT_VER_NUMBER}" APPEND)
