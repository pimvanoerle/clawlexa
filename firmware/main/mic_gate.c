#include "mic_gate.h"

#include <string.h>

int64_t mic_gate_update(int64_t mute_until_us, const char *frame,
                        int64_t now_us, int64_t tail_us) {
    if (frame == NULL) {
        return mute_until_us;
    }
    if (strstr(frame, "play_begin") != NULL) {
        return INT64_MAX;  /* muted until play_end releases it */
    }
    if (strstr(frame, "play_end") != NULL) {
        return now_us + tail_us;
    }
    return mute_until_us;
}

bool mic_gate_muted(int64_t mute_until_us, int64_t now_us) {
    return now_us < mute_until_us;
}
