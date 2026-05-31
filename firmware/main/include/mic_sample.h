#pragma once

#include <stdint.h>

/* Convert one raw I2S sample from the ICS-43434 (24-bit, left-justified in a
 * 32-bit slot) to 16-bit PCM, with a fixed gain and clamping. Pure (no IO) so
 * it's host-tested. */
int16_t mic_sample_to_pcm16(int32_t raw);
