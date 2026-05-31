#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Parsed view of a PCM WAV blob (header + a pointer into the data chunk).
 * `data` points inside the input buffer — no copy. */
typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    const uint8_t *data;   /* start of PCM samples within the input buffer */
    uint32_t data_bytes;   /* length of the data chunk */
} wav_info_t;

/* Parse a canonical RIFF/WAVE PCM blob: validates the RIFF+WAVE tags, walks the
 * chunks to read `fmt ` and locate `data`, and bounds-checks against `len`.
 * Pure (no IDF/IO) so it's host-tested. Returns false on any malformation. */
bool wav_parse(const uint8_t *buf, size_t len, wav_info_t *out);
