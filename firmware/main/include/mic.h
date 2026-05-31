#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Phase 1c-b capture bring-up: ICS-43434 I2S MEMS mic on its own I2S bus (no
 * I2C). Logs "audio: ICS-43434 ready". */
esp_err_t mic_init(void);

/* Read up to max_samples of mono 16-bit PCM from the mic (blocking, with a
 * timeout). Returns the number of samples read, or <0 on error. */
int mic_read_samples(int16_t *out, int max_samples);

/* Record `seconds` of mono 16-bit/16 kHz audio and dump it over the serial
 * console as base64, framed by MIC_WAV_BEGIN/END markers, for the host script
 * tools/capture_mic.py to reconstruct into a .wav. Logs "audio: capture done". */
esp_err_t mic_capture_and_dump(uint32_t seconds);
