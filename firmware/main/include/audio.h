#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

/* Phase 1c playback bring-up: ES8311 over I2S, control on the shared I2C bus.
 * Opens the codec at 16 kHz / 16-bit / mono and enables the speaker amp.
 * Logs "audio: ES8311 ready". */
esp_err_t audio_play_init(i2c_master_bus_handle_t i2c_bus);

/* Play a sine test tone (blocking) for duration_ms. Requires audio_play_init. */
esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);
