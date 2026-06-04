#include "mic_gate.h"

#include <stdlib.h>
#include <string.h>

/* Read an integer JSON field value, e.g. key="\"ms\":" from {...,"ms":1234,...}.
 * Returns -1 if the key is absent or not followed by a number. */
static long json_int_field(const char *s, const char *key) {
    const char *p = strstr(s, key);
    if (p == NULL) {
        return -1;
    }
    p += strlen(key);
    while (*p == ' ') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return -1;
    }
    return strtol(p, NULL, 10);
}

int64_t mic_gate_update(int64_t mute_until_us, const char *frame,
                        int64_t now_us, int64_t tail_us) {
    if (frame == NULL) {
        return mute_until_us;
    }
    if (strstr(frame, "play_begin") != NULL) {
        /* Mute for the whole clip (+ tail). play_begin arrives when the audio is
         * queued, so "now + duration" closely tracks when the speaker finishes. */
        long ms = json_int_field(frame, "\"ms\":");
        if (ms >= 0) {
            return now_us + (int64_t)ms * 1000 + tail_us;
        }
        return INT64_MAX;  /* unknown length: hold until play_end */
    }
    if (strstr(frame, "play_end") != NULL) {
        int64_t floor = now_us + tail_us;
        if (mute_until_us == INT64_MAX) {
            return floor;  /* we were holding for an unknown-length clip */
        }
        /* A known-duration mute is already set — don't shorten it. */
        return mute_until_us > floor ? mute_until_us : floor;
    }
    return mute_until_us;
}

bool mic_gate_muted(int64_t mute_until_us, int64_t now_us) {
    return now_us < mute_until_us;
}
