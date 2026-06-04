#include "touch_geom.h"

bool touch_in_circle(int16_t x, int16_t y, int16_t cx, int16_t cy,
                     uint16_t radius) {
    int32_t dx = (int32_t)x - cx;
    int32_t dy = (int32_t)y - cy;
    /* Compare squared distances to avoid a sqrt (and any float). */
    return dx * dx + dy * dy <= (int32_t)radius * radius;
}

bool touch_int_active(int int_level, int active_level) {
    return int_level == active_level;
}
