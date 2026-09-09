#include "pcm_carry.h"

#include <string.h>

void pcm_carry_reset(pcm_carry_t *c) {
    c->byte = 0;
    c->pending = false;
}

size_t pcm_carry_feed(pcm_carry_t *c, const uint8_t *data, size_t len,
                      int16_t *out, size_t out_cap, size_t *consumed) {
    size_t used = 0, made = 0;

    /* Pair a byte held over from the previous chunk with the first byte here.
     * Little-endian: the carried byte is the low half of the sample. */
    if (c->pending && len > 0 && made < out_cap) {
        uint8_t pair[2] = { c->byte, data[0] };
        memcpy(&out[made], pair, 2);
        made++;
        used = 1;
        c->pending = false;
    }

    /* Whole samples, copied rather than cast: `data` has no alignment guarantee
     * and Xtensa faults on an unaligned 16-bit load. */
    while (len - used >= 2 && made < out_cap) {
        memcpy(&out[made], data + used, 2);
        made++;
        used += 2;
    }

    /* One byte left over and room to look at it: hold it for the next chunk
     * rather than dropping it. Only when the output is not the limit, or we
     * would carry a byte the caller still has to send us again. */
    if (len - used == 1 && made < out_cap) {
        c->byte = data[used];
        c->pending = true;
        used += 1;
    }

    *consumed = used;
    return made;
}
