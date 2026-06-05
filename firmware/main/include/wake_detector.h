#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On-device wake word (Phase 4b) — microWakeWord pipeline: the micro_speech
 * spectrogram frontend feeding a streaming INT8 TFLite-Micro model, VAD-gated.
 * Pure detection; the wake_gate state machine consumes the result.
 *
 * wake_detector_init() sets up the frontend + the wake + VAD interpreters
 * (returns false on allocation/model error). wake_detector_feed() takes 16 kHz
 * mono 16-bit PCM, runs the frontend + models over every complete 10 ms slice in
 * the buffer, and returns true if the wake word fired on this call (VAD-gated).
 * Not thread-safe: call both from one task. */
bool wake_detector_init(void);
bool wake_detector_feed(const int16_t *samples, size_t count);

#ifdef __cplusplus
}
#endif
