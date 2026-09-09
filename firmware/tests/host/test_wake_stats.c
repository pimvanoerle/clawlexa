#include "unity.h"
#include "wake_stats.h"

/* Wake-path timing arithmetic (Phase 4c, tier T2). Pure — no TFLite, no board. */

static wake_stage_stats_t s;

void setUp(void) { wake_stats_reset(&s); }
void tearDown(void) {}

void test_starts_empty(void) {
    TEST_ASSERT_EQUAL_UINT(0, s.runs);
    TEST_ASSERT_EQUAL_UINT(0, wake_stats_mean_us(&s));
    TEST_ASSERT_EQUAL_UINT(0, s.max_us);
}

void test_mean_of_no_runs_is_zero_not_a_divide_by_zero(void) {
    TEST_ASSERT_EQUAL_UINT(0, wake_stats_mean_us(&s));
    TEST_ASSERT_EQUAL_UINT(0, wake_stats_per_slice_us(&s, 0));
}

void test_accumulates_mean_and_max(void) {
    wake_stats_add(&s, 100);
    wake_stats_add(&s, 300);
    wake_stats_add(&s, 200);
    TEST_ASSERT_EQUAL_UINT(3, s.runs);
    TEST_ASSERT_EQUAL_UINT(200, wake_stats_mean_us(&s));
    TEST_ASSERT_EQUAL_UINT(300, s.max_us);   /* the worst run breaks the budget */
}

void test_per_slice_differs_from_per_run(void) {
    /* A model that invokes every 3rd slice: expensive per invoke, cheap per
     * slice. Confusing the two is how you'd wrongly conclude a model doesn't
     * fit. */
    wake_stats_add(&s, 900);
    wake_stats_add(&s, 900);
    TEST_ASSERT_EQUAL_UINT(900, wake_stats_mean_us(&s));       /* per invoke */
    TEST_ASSERT_EQUAL_UINT(300, wake_stats_per_slice_us(&s, 6)); /* per slice */
}

void test_budget_percent(void) {
    TEST_ASSERT_EQUAL_UINT(25, wake_stats_budget_pct(2500, 10000));
    TEST_ASSERT_EQUAL_UINT(0, wake_stats_budget_pct(0, 10000));
}

void test_budget_over_100_is_reported_not_clamped(void) {
    /* The whole point of the meter: a detector that has fallen behind real time
     * must say so, not report a comfortable 100%. */
    TEST_ASSERT_EQUAL_UINT(140, wake_stats_budget_pct(14000, 10000));
}

void test_budget_with_no_budget_does_not_divide_by_zero(void) {
    TEST_ASSERT_EQUAL_UINT(0, wake_stats_budget_pct(500, 0));
}

void test_reset_clears_everything(void) {
    wake_stats_add(&s, 500);
    wake_stats_reset(&s);
    TEST_ASSERT_EQUAL_UINT(0, s.runs);
    TEST_ASSERT_EQUAL_UINT(0, s.max_us);
    TEST_ASSERT_EQUAL_UINT(0, wake_stats_mean_us(&s));
}

void test_totals_survive_a_long_run(void) {
    /* 24 h at 100 slices/s with a 1 ms stage overflows uint32 microseconds —
     * total_us is deliberately 64-bit. */
    for (int i = 0; i < 100000; i++) {
        wake_stats_add(&s, 1000);
    }
    TEST_ASSERT_EQUAL_UINT64(100000000ULL, s.total_us);
    TEST_ASSERT_EQUAL_UINT(1000, wake_stats_mean_us(&s));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_mean_of_no_runs_is_zero_not_a_divide_by_zero);
    RUN_TEST(test_accumulates_mean_and_max);
    RUN_TEST(test_per_slice_differs_from_per_run);
    RUN_TEST(test_budget_percent);
    RUN_TEST(test_budget_over_100_is_reported_not_clamped);
    RUN_TEST(test_budget_with_no_budget_does_not_divide_by_zero);
    RUN_TEST(test_reset_clears_everything);
    RUN_TEST(test_totals_survive_a_long_run);
    return UNITY_END();
}
