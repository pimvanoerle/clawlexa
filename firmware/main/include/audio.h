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

/* --- streamed playback (bridge audio) -----------------------------------
 *
 * PCM arriving from the bridge is buffered, not written straight to I2S. The
 * I2S DMA is only 1440 samples deep (90 ms at 16 kHz) and the channel is
 * `auto_clear`, so an empty DMA plays *silence* — any WiFi hiccup longer than
 * 90 ms was an audible gap in the middle of a word. A ~2 s ring buffer plus a
 * dedicated playback task absorbs that jitter (SPEC §6).
 *
 * Call order per clip: audio_play_begin() -> audio_play_pcm() xN ->
 * audio_play_end(). The playback task waits for a small pre-roll before it
 * starts, so the buffer has a cushion in hand rather than starting empty.
 */

/* Start a clip: drop anything stale and arm the playback task. */
void audio_play_begin(void);

/* Queue 16-bit mono PCM for playback at the current I2S rate. Blocks only while
 * the buffer is full — i.e. when we are already seconds ahead — so audio is
 * never dropped, but the network is never throttled by the speaker either. */
esp_err_t audio_play_pcm(const int16_t *samples, size_t n_samples);

/* The clip is fully sent: let the task drain the buffer and report on it. */
void audio_play_end(void);

/* Per-clip playback health, logged at the end of each clip and readable for
 * tests: how much was played, and how often the buffer ran dry mid-clip
 * (an underrun is a gap you can hear). */
typedef struct {
    uint32_t samples;        /* samples handed to I2S */
    uint32_t underruns;      /* times the buffer was empty mid-clip */
    uint32_t dropped;        /* samples lost to a full buffer (should be 0) */
    uint32_t max_level;      /* deepest the buffer ever got, in samples */
    uint32_t preroll_ms;     /* how long we waited before starting */
} audio_play_stats_t;

audio_play_stats_t audio_play_get_stats(void);
