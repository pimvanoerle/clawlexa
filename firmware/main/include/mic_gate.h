#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Half-duplex mic gating (pure, host-tested).
 *
 * The bridge brackets a spoken reply with play_begin .. play_end control
 * frames. While that playback is in flight — and for a short tail afterward, to
 * let the speaker and room settle — the device mutes its mic so it does not
 * capture and re-transcribe its own output (SPEC §6: half-duplex for v1).
 *
 * mic_gate_update folds one incoming control frame into the current mute
 * deadline (microseconds, same clock as now_us):
 *   - frame contains "play_begin" -> muted until released (INT64_MAX)
 *   - frame contains "play_end"   -> muted until now_us + tail_us
 *   - any other frame (or NULL)   -> deadline unchanged
 * mic_gate_muted reports whether the mic is muted at now_us. */
int64_t mic_gate_update(int64_t mute_until_us, const char *frame,
                        int64_t now_us, int64_t tail_us);
bool mic_gate_muted(int64_t mute_until_us, int64_t now_us);
