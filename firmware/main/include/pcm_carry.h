#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reassemble 16-bit PCM from a byte stream that arrives in arbitrary chunks
 * (pure, host-tested).
 *
 * The bridge sends tidy 1024-byte frames, but the ESP WebSocket client hands
 * them over in whatever pieces TCP and its receive buffer produce — 209 bytes,
 * then 815, then 425. Treating each chunk as standalone PCM and taking
 * `len / 2` samples silently discards the trailing byte of every odd chunk, and
 * from then on every sample is assembled from the wrong pair of bytes: the low
 * byte of one and the high byte of the next. That is the intermittent crackle,
 * and it is intermittent precisely because a later odd chunk shifts it back.
 *
 * So a trailing odd byte is *carried* and paired with the first byte of the next
 * chunk. Reset between clips so a stray byte can't leak across a boundary.
 */
typedef struct {
    uint8_t byte;   /* the carried low byte, valid only when `pending` */
    bool pending;
} pcm_carry_t;

void pcm_carry_reset(pcm_carry_t *c);

/* Convert bytes to whole samples, carrying across calls.
 *
 * Writes at most `out_cap` samples into `out`, sets `*consumed` to the input
 * bytes used, and returns the samples written. Call again with the remainder
 * while `*consumed < len` — a caller with a small scratch buffer can stream a
 * chunk of any size through it. */
size_t pcm_carry_feed(pcm_carry_t *c, const uint8_t *data, size_t len,
                      int16_t *out, size_t out_cap, size_t *consumed);
