#include "st77916_variant.h"

st77916_variant_t st77916_variant_from_id(const uint8_t id[4]) {
    if (id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
        return ST77916_VARIANT_NEW;
    }
    return ST77916_VARIANT_DEFAULT;
}
