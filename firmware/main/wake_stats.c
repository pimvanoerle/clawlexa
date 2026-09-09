#include "wake_stats.h"

void wake_stats_reset(wake_stage_stats_t *s) {
    s->runs = 0;
    s->total_us = 0;
    s->max_us = 0;
}

void wake_stats_add(wake_stage_stats_t *s, uint32_t us) {
    s->runs++;
    s->total_us += us;
    if (us > s->max_us) {
        s->max_us = us;
    }
}

uint32_t wake_stats_mean_us(const wake_stage_stats_t *s) {
    return s->runs ? (uint32_t)(s->total_us / s->runs) : 0;
}

uint32_t wake_stats_per_slice_us(const wake_stage_stats_t *s, uint32_t slices) {
    return slices ? (uint32_t)(s->total_us / slices) : 0;
}

uint32_t wake_stats_budget_pct(uint32_t used_us_per_slice, uint32_t slice_budget_us) {
    if (slice_budget_us == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)used_us_per_slice * 100) / slice_budget_us);
}
