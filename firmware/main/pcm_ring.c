#include "pcm_ring.h"

#include <string.h>

void pcm_ring_init(pcm_ring_t *r, int16_t *storage, size_t cap_samples) {
    r->buf = storage;
    r->cap = cap_samples;
    r->head = 0;
    r->tail = 0;
    r->dropped = 0;
}

void pcm_ring_reset(pcm_ring_t *r) {
    r->head = 0;
    r->tail = 0;
    r->dropped = 0;
}

size_t pcm_ring_level(const pcm_ring_t *r) {
    size_t head = r->head, tail = r->tail;
    return head >= tail ? head - tail : r->cap - tail + head;
}

size_t pcm_ring_free(const pcm_ring_t *r) {
    return r->cap - 1 - pcm_ring_level(r);  /* one slot kept empty */
}

size_t pcm_ring_write(pcm_ring_t *r, const int16_t *src, size_t n) {
    size_t space = pcm_ring_free(r);
    if (n > space) {
        n = space;  /* short write; whether the rest is retried or dropped is
                     * the caller's call, and only the caller can count it */
    }
    size_t head = r->head;
    size_t first = r->cap - head;      /* room before wrapping */
    if (first > n) {
        first = n;
    }
    memcpy(r->buf + head, src, first * sizeof(int16_t));
    if (n > first) {
        memcpy(r->buf, src + first, (n - first) * sizeof(int16_t));
    }
    r->head = (head + n) % r->cap;     /* publish only after the copy */
    return n;
}

size_t pcm_ring_read(pcm_ring_t *r, int16_t *dst, size_t n) {
    size_t level = pcm_ring_level(r);
    if (n > level) {
        n = level;
    }
    size_t tail = r->tail;
    size_t first = r->cap - tail;
    if (first > n) {
        first = n;
    }
    memcpy(dst, r->buf + tail, first * sizeof(int16_t));
    if (n > first) {
        memcpy(dst + first, r->buf, (n - first) * sizeof(int16_t));
    }
    r->tail = (tail + n) % r->cap;     /* release space only after the copy */
    return n;
}
