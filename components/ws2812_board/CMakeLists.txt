# firmware/components/ws2812_board/CMakeLists.txt
# The esp-matter "device" component compiles device.c from
# device_hal/device/${device_type}/, which only works for boards shipped
# inside esp-matter. We keep this board profile as a standalone component
# (auto-discovered from firmware/components/) so device.c is built without
# touching ~/.espressif.
idf_component_register(SRC_DIRS "." REQUIRES led_driver iot_button)
