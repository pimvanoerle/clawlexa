#include "audio_tone.h"

#include <math.h>

#define TWO_PI 6.28318530717958647692

void audio_fill_sine(int16_t *buf, size_t n, uint32_t freq_hz,
                     uint32_t sample_rate, int16_t amplitude,
                     uint32_t start_sample) {
    const double w = TWO_PI * (double)freq_hz / (double)sample_rate;
    for (size_t i = 0; i < n; i++) {
        const double s = sin(w * (double)(start_sample + i));
        buf[i] = (int16_t)lround(s * (double)amplitude);
    }
}
