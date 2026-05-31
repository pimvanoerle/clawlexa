#include "mic_sample.h"

/* The ICS-43434 puts a 24-bit sample in the top bits of the 32-bit slot.
 * raw >> 16 would be the unity 24->16 truncation; we shift less to add ~8x of
 * gain (the mic is quiet at conversational distance), then clamp. Tune here if
 * recordings clip (raise) or are too quiet (lower). */
#define MIC_GAIN_SHIFT 13

int16_t mic_sample_to_pcm16(int32_t raw) {
    int32_t v = raw >> MIC_GAIN_SHIFT;  /* arithmetic shift preserves sign */
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)v;
}
