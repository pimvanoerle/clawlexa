#include "wav.h"

#include <string.h>

/* Little-endian readers (WAV is little-endian). */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool wav_parse(const uint8_t *buf, size_t len, wav_info_t *out) {
    if (buf == NULL || out == NULL || len < 12) {
        return false;
    }
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool have_fmt = false, have_data = false;
    size_t pos = 12;  /* first chunk after the RIFF/WAVE header */
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        uint32_t chunk_len = rd32(buf + pos + 4);
        size_t body = pos + 8;
        if (body + chunk_len > len) {
            return false;  /* chunk runs past the buffer */
        }

        if (memcmp(id, "fmt ", 4) == 0) {
            if (chunk_len < 16) {
                return false;
            }
            out->channels = rd16(buf + body + 2);
            out->sample_rate = rd32(buf + body + 4);
            out->bits_per_sample = rd16(buf + body + 14);
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            out->data = buf + body;
            out->data_bytes = chunk_len;
            have_data = true;
        }

        /* chunks are word-aligned: pad odd lengths */
        pos = body + chunk_len + (chunk_len & 1u);
    }
    return have_fmt && have_data;
}
