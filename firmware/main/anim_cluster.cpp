#include "anim_cluster.h"
using namespace esp_matter;
static uint8_t s_hash[32] = {0}, s_meta[6] = {0}, s_frame[1100] = {0};
static uint8_t s_cmd[1] = {0}, s_active[32] = {0}, s_cached[160] = {0};

cluster_t *anim::anim_cluster_create(endpoint_t *ep) {
    cluster_t *c = cluster::create(ep, CLUSTER_ID, CLUSTER_FLAG_SERVER);
    attribute::create(c, ATTR_MATRIX_WIDTH, ATTRIBUTE_FLAG_NONE, esp_matter_uint16(CONFIG_MATRIX_WIDTH));
    attribute::create(c, ATTR_MATRIX_HEIGHT, ATTRIBUTE_FLAG_NONE, esp_matter_uint16(CONFIG_MATRIX_HEIGHT));
    attribute::create(c, ATTR_PIXEL_COUNT, ATTRIBUTE_FLAG_NONE, esp_matter_uint16(CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT));
    attribute::create(c, ATTR_SERPENTINE, ATTRIBUTE_FLAG_NONE, esp_matter_bool(CONFIG_MATRIX_SERPENTINE));
    attribute::create(c, ATTR_CACHED, ATTRIBUTE_FLAG_NONE, esp_matter_octet_str(s_cached, 160), 160);
    attribute::create(c, ATTR_TRANSFER_HASH, ATTRIBUTE_FLAG_WRITABLE, esp_matter_octet_str(s_hash, 32), 32);
    attribute::create(c, ATTR_TRANSFER_META, ATTRIBUTE_FLAG_WRITABLE, esp_matter_octet_str(s_meta, 6), 6);
    attribute::create(c, ATTR_FRAME_CHUNK, ATTRIBUTE_FLAG_WRITABLE, esp_matter_octet_str(s_frame, 1100), 1100);
    attribute::create(c, ATTR_STATUS, ATTRIBUTE_FLAG_NONE, esp_matter_enum8(0));
    attribute::create(c, ATTR_PLAY_CMD, ATTRIBUTE_FLAG_WRITABLE, esp_matter_octet_str(s_cmd, 1), 1);
    attribute::create(c, ATTR_ACTIVE, ATTRIBUTE_FLAG_NONE, esp_matter_octet_str(s_active, 32), 32);
    return c;
}
