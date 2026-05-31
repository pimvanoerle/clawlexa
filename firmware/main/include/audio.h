#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Phase 1c playback bring-up: master I2S TX to the PCM5101A DAC (no I2C codec).
 * Opens at 16 kHz / 16-bit / mono. Logs "audio: PCM5101 ready". */
esp_err_t audio_play_init(void);

/* Play a sine test tone (blocking) for duration_ms. Requires audio_play_init. */
esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);

/* Play a 16-bit mono PCM WAV blob (blocking). Retunes the I2S clock to the
 * WAV's sample rate. Requires audio_play_init. */
esp_err_t audio_play_wav(const uint8_t *wav, size_t len);
