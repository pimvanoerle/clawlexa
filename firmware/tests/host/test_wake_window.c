#include "unity.h"
#include "wake_window.h"

/* The wake verdict (Phase 4c, tier T2): a sliding-window average over a cutoff,
 * plus a refractory period so one spoken phrase fires once. No TFLite, no board. */

#define SIZE 5
#define CUTOFF 128
#define REFRACTORY 3

static wake_window_t w;
static uint8_t storage[SIZE];

void setUp(void) { wake_window_init(&w, storage, SIZE, CUTOFF, REFRACTORY); }
void tearDown(void) {}

/* Push `n` samples of `p`, which also walks the refractory down when quiet. */
static void push_n(uint8_t p, int n) {
    for (int i = 0; i < n; i++) wake_window_push(&w, p);
}

void test_starts_silent_and_refractory(void) {
    TEST_ASSERT_FALSE(wake_window_fired(&w));
    TEST_ASSERT_FALSE(wake_window_active(&w));
}

void test_a_single_spike_is_not_a_detection(void) {
    /* One loud slice among quiet ones cannot carry the average — this is the
     * whole reason for a window rather than a threshold. */
    push_n(0, REFRACTORY);          /* clear the refractory */
    wake_window_push(&w, 255);
    TEST_ASSERT_FALSE(wake_window_fired(&w));
}

void test_a_sustained_phrase_fires(void) {
    push_n(0, REFRACTORY);
    push_n(200, SIZE);              /* fill the window above cutoff */
    TEST_ASSERT_TRUE(wake_window_fired(&w));
}

void test_refractory_blocks_an_immediate_refire(void) {
    push_n(0, REFRACTORY);
    push_n(200, SIZE);
    TEST_ASSERT_TRUE(wake_window_fired(&w));
    wake_window_reset(&w);          /* what the caller does after a wake */
    push_n(200, SIZE);              /* still loud: the phrase is decaying out */
    TEST_ASSERT_FALSE(wake_window_fired(&w));
}

void test_only_quiet_slices_retire_the_refractory(void) {
    /* Continuous noise must not re-arm the model, or it fires the moment it
     * dips. Loud slices leave the refractory exactly where it was. */
    wake_window_reset(&w);
    push_n(255, 50);
    TEST_ASSERT_FALSE(wake_window_fired(&w));
    push_n(0, REFRACTORY);          /* now genuinely quiet */
    push_n(200, SIZE);
    TEST_ASSERT_TRUE(wake_window_fired(&w));
}

void test_active_ignores_the_refractory(void) {
    /* The VAD gate is a continuous signal, not an event: it must report speech
     * even while a wake model is still refractory. */
    wake_window_reset(&w);
    push_n(200, SIZE);
    TEST_ASSERT_FALSE(wake_window_fired(&w));   /* refractory */
    TEST_ASSERT_TRUE(wake_window_active(&w));   /* but plainly active */
}

void test_average_not_maximum(void) {
    /* Just at the boundary: a window that averages exactly the cutoff must NOT
     * fire (the test is strictly greater), one above it must. */
    push_n(0, REFRACTORY);
    push_n(CUTOFF, SIZE);
    TEST_ASSERT_FALSE(wake_window_fired(&w));
    push_n(CUTOFF + 1, SIZE);
    TEST_ASSERT_TRUE(wake_window_fired(&w));
}

void test_latest_reports_the_most_recent_probability(void) {
    wake_window_push(&w, 42);
    TEST_ASSERT_EQUAL_UINT8(42, wake_window_latest(&w));
    wake_window_push(&w, 200);
    TEST_ASSERT_EQUAL_UINT8(200, wake_window_latest(&w));
}

void test_reset_clears_the_window(void) {
    push_n(0, REFRACTORY);
    push_n(255, SIZE);
    TEST_ASSERT_TRUE(wake_window_active(&w));
    wake_window_reset(&w);
    TEST_ASSERT_FALSE(wake_window_active(&w));
    TEST_ASSERT_EQUAL_UINT8(0, wake_window_latest(&w));
}

void test_two_models_with_different_cutoffs_are_independent(void) {
    /* Phase 4c: each phrase carries its own cutoff from its own manifest, so a
     * strict model and a lenient one must judge the same probabilities
     * differently. */
    uint8_t s1[SIZE], s2[SIZE];
    wake_window_t strict, lenient;
    wake_window_init(&strict, s1, SIZE, 247, REFRACTORY);   /* okay_nabu-ish 0.97 */
    wake_window_init(&lenient, s2, SIZE, 128, REFRACTORY);  /* hey_pinchy-ish 0.50 */
    for (int i = 0; i < REFRACTORY; i++) {
        wake_window_push(&strict, 0);
        wake_window_push(&lenient, 0);
    }
    for (int i = 0; i < SIZE; i++) {
        wake_window_push(&strict, 200);
        wake_window_push(&lenient, 200);
    }
    TEST_ASSERT_FALSE(wake_window_fired(&strict));
    TEST_ASSERT_TRUE(wake_window_fired(&lenient));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_silent_and_refractory);
    RUN_TEST(test_a_single_spike_is_not_a_detection);
    RUN_TEST(test_a_sustained_phrase_fires);
    RUN_TEST(test_refractory_blocks_an_immediate_refire);
    RUN_TEST(test_only_quiet_slices_retire_the_refractory);
    RUN_TEST(test_active_ignores_the_refractory);
    RUN_TEST(test_average_not_maximum);
    RUN_TEST(test_latest_reports_the_most_recent_probability);
    RUN_TEST(test_reset_clears_the_window);
    RUN_TEST(test_two_models_with_different_cutoffs_are_independent);
    return UNITY_END();
}
