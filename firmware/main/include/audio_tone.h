#pragma once

#include <stddef.h>
#include <stdint.h>

/* Fill `buf` with `n` mono 16-bit PCM samples of a sine wave at freq_hz for the
 * given sample_rate and peak amplitude. `start_sample` is the absolute sample
 * index of buf[0], so consecutive buffers stay phase-continuous when streaming.
 * Pure (math only), no IDF/IO — host-tested. */
void audio_fill_sine(int16_t *buf, size_t n, uint32_t freq_hz,
                     uint32_t sample_rate, int16_t amplitude,
                     uint32_t start_sample);
