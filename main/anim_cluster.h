#pragma once
#include <esp_matter.h>
namespace anim {
constexpr uint32_t CLUSTER_ID = 0x1618FC01;
constexpr uint32_t ATTR_MATRIX_WIDTH = 0x0000, ATTR_MATRIX_HEIGHT = 0x0001, ATTR_PIXEL_COUNT = 0x0002, ATTR_SERPENTINE = 0x0003;
constexpr uint32_t ATTR_CACHED = 0x0004, ATTR_TRANSFER_HASH = 0x0005, ATTR_TRANSFER_META = 0x0006, ATTR_FRAME_CHUNK = 0x0007;
constexpr uint32_t ATTR_STATUS = 0x0008, ATTR_PLAY_CMD = 0x0009, ATTR_ACTIVE = 0x000A;
esp_matter::cluster_t *anim_cluster_create(esp_matter::endpoint_t *ep);
}
