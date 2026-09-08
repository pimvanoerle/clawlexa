#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Single-producer/single-consumer PCM ring buffer (pure, host-tested).
 *
 * Sits between the network and the speaker. Without it, each WebSocket frame's
 * PCM went straight into the I2S DMA, whose entire depth is 1440 samples — 90 ms
 * at 16 kHz. Any WiFi hiccup longer than that drained the DMA, and because the
 * channel is configured `auto_clear`, an empty DMA emits *silence*: the gaps we
 * hear are literally zeros (SPEC §6).
 *
 * The producer is the WebSocket event handler; the consumer is the playback
 * task. One writer and one reader means no lock is needed: each side only
 * advances its own index, and the other index is read once per operation.
 *
 * One slot is always left empty so head==tail unambiguously means "empty".
 */
typedef struct {
    int16_t *buf;
    size_t cap;             /* storage size in samples (usable = cap - 1) */
    volatile size_t head;   /* producer writes here */
    volatile size_t tail;   /* consumer reads here */
    size_t dropped;         /* samples the CALLER chose to discard; the ring
                             * never fills this in — a short write only means
                             * "wait", and counting that as loss reads like
                             * audio was thrown away when none was */
} pcm_ring_t;

/* `storage` must hold `cap_samples` int16_t and outlive the ring. */
void pcm_ring_init(pcm_ring_t *r, int16_t *storage, size_t cap_samples);

/* Discard any buffered audio and clear the drop counter (new clip). */
void pcm_ring_reset(pcm_ring_t *r);

/* Copy up to `n` samples in; returns how many fit. A short return means the
 * consumer is behind — the caller decides whether to wait or drop, and is the
 * only one that can meaningfully count a drop. */
size_t pcm_ring_write(pcm_ring_t *r, const int16_t *src, size_t n);

/* Copy up to `n` samples out; returns how many were available. */
size_t pcm_ring_read(pcm_ring_t *r, int16_t *dst, size_t n);

/* Samples currently buffered, and space left for more. */
size_t pcm_ring_level(const pcm_ring_t *r);
size_t pcm_ring_free(const pcm_ring_t *r);
