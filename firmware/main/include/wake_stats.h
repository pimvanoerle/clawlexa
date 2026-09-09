#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Timing accumulators for the wake-detection path (pure, host-tested).
 *
 * Every 10 ms slice runs the preprocessor plus one inference per streaming
 * model, and all of it has to finish inside that 10 ms or the detector falls
 * behind real time. Neither RAM nor flash limits how many wake phrases the
 * device can listen for — arenas live in PSRAM and models are ~59 KB against
 * ~1.4 MB free — but **CPU does**, and nothing measured it. This is the meter
 * that tells us how many models fit (SPEC §7, Phase 4c).
 *
 * Kept separate from the detector so the arithmetic is testable without a
 * board, a model, or TFLite.
 */
typedef struct {
    uint32_t runs;       /* times this stage actually executed */
    uint64_t total_us;   /* cumulative microseconds spent in it */
    uint32_t max_us;     /* worst single run — the number that breaks a budget */
} wake_stage_stats_t;

void wake_stats_reset(wake_stage_stats_t *s);
void wake_stats_add(wake_stage_stats_t *s, uint32_t us);

/* Mean microseconds per execution. 0 when it never ran. */
uint32_t wake_stats_mean_us(const wake_stage_stats_t *s);

/* Microseconds per *slice*, which is not the same as per run: a streaming model
 * only invokes every `stride` slices, so a model that is expensive but rarely
 * invoked can still be cheap per slice. This is the figure that adds up across
 * models into the budget. 0 slices -> 0. */
uint32_t wake_stats_per_slice_us(const wake_stage_stats_t *s, uint32_t slices);

/* What share of the real-time budget a per-slice cost consumes, in percent.
 * Can exceed 100 — that is the interesting case, and it must not be clamped or
 * a detector that has fallen behind would report a comfortable 100%. */
uint32_t wake_stats_budget_pct(uint32_t used_us_per_slice, uint32_t slice_budget_us);

#ifdef __cplusplus
}
#endif
