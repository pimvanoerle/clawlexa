#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The "has it fired?" verdict for one streaming wake-word model (pure,
 * host-tested).
 *
 * A microWakeWord model emits a probability per inference, and a single spike is
 * not a detection: the verdict is the average over a short sliding window
 * exceeding the model's cutoff. On top of that sits a refractory period — after
 * a fire (or a reset) the model must see a minimum number of slices before it
 * may fire again, otherwise one spoken phrase retriggers repeatedly as it
 * decays out of the window.
 *
 * Extracted from StreamModel for Phase 4c: with several phrases live at once
 * this logic runs per model, and it is the only part of wake detection that
 * needs neither a board nor TFLite to test.
 */
typedef struct {
    uint8_t *probs;   /* caller-owned storage, `size` entries */
    int size;
    int idx;          /* most recently written slot */
    int ignore;       /* < 0 while refractory; climbs back toward 0 */
    int refractory;   /* slices to wait after a reset before firing again */
    uint8_t cutoff;   /* 0-255; the manifest's probability_cutoff, scaled */
} wake_window_t;

void wake_window_init(wake_window_t *w, uint8_t *storage, int size,
                      uint8_t cutoff, int refractory);

/* Clear the window and re-arm the refractory period. */
void wake_window_reset(wake_window_t *w);

/* Record one probability. Below-cutoff samples also let the refractory expire,
 * so a model held above cutoff by continuous noise cannot re-arm itself. */
void wake_window_push(wake_window_t *w, uint8_t prob);

/* Windowed average over cutoff AND out of the refractory period — the wake
 * verdict. */
bool wake_window_fired(const wake_window_t *w);

/* Windowed average over cutoff, ignoring the refractory. Used by the VAD gate,
 * which is a continuous "is anyone speaking" signal rather than an event. */
bool wake_window_active(const wake_window_t *w);

/* Most recent probability — for logging the confidence of a detection. */
uint8_t wake_window_latest(const wake_window_t *w);

#ifdef __cplusplus
}
#endif
