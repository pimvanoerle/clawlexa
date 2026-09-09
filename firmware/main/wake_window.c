#include "wake_window.h"

#include <string.h>

static uint32_t windowed_sum(const wake_window_t *w) {
    uint32_t sum = 0;
    for (int i = 0; i < w->size; i++) {
        sum += w->probs[i];
    }
    return sum;
}

void wake_window_init(wake_window_t *w, uint8_t *storage, int size,
                      uint8_t cutoff, int refractory) {
    w->probs = storage;
    w->size = size;
    w->cutoff = cutoff;
    w->refractory = refractory;
    wake_window_reset(w);
}

void wake_window_reset(wake_window_t *w) {
    memset(w->probs, 0, (size_t) w->size);
    w->idx = 0;
    w->ignore = -w->refractory;
}

void wake_window_push(wake_window_t *w, uint8_t prob) {
    w->idx = (w->idx + 1) % w->size;
    w->probs[w->idx] = prob;
    if (prob < w->cutoff) {
        /* Only quiet slices retire the refractory. Counting loud ones would let
         * continuous noise re-arm the model and fire the moment it dips. */
        if (w->ignore < 0) {
            w->ignore++;
        }
    }
}

bool wake_window_fired(const wake_window_t *w) {
    if (w->ignore < 0) {
        return false;
    }
    return wake_window_active(w);
}

bool wake_window_active(const wake_window_t *w) {
    return windowed_sum(w) > (uint32_t) w->cutoff * (uint32_t) w->size;
}

uint8_t wake_window_latest(const wake_window_t *w) {
    return w->probs[w->idx];
}
