#include "unity.h"
#include "mic_gate.h"

/* Half-duplex mic gating (3c, tier T2). Tail is in microseconds. */

#define TAIL 300000  /* 300 ms */

void setUp(void) {}
void tearDown(void) {}

void test_idle_is_not_muted(void) {
    TEST_ASSERT_FALSE(mic_gate_muted(0, 1000));
}

void test_play_begin_with_ms_mutes_for_clip_plus_tail(void) {
    /* 500 ms clip at t=1000us -> muted through 1000 + 500000 + TAIL. */
    int64_t m = mic_gate_update(0, "{\"type\":\"play_begin\",\"rate\":16000,\"ms\":500}",
                                1000, TAIL);
    TEST_ASSERT_TRUE(mic_gate_muted(m, 1000));
    TEST_ASSERT_TRUE(mic_gate_muted(m, 1000 + 500000 + TAIL - 1));  /* still playing */
    TEST_ASSERT_FALSE(mic_gate_muted(m, 1000 + 500000 + TAIL));     /* clip+tail done */
}

void test_play_begin_without_ms_holds_until_end(void) {
    int64_t m = mic_gate_update(0, "{\"type\":\"play_begin\",\"rate\":16000}", 1000, TAIL);
    TEST_ASSERT_TRUE(mic_gate_muted(m, 1000));
    TEST_ASSERT_TRUE(mic_gate_muted(m, 999999999));  /* no ms, no end yet -> muted */
}

void test_play_end_releases_an_unknown_length_hold(void) {
    int64_t m = mic_gate_update(INT64_MAX, "{\"type\":\"play_end\"}", 1000, TAIL);
    TEST_ASSERT_TRUE(mic_gate_muted(m, 1000 + TAIL - 1));  /* tail */
    TEST_ASSERT_FALSE(mic_gate_muted(m, 1000 + TAIL));     /* released */
}

void test_play_end_does_not_shorten_a_duration_mute(void) {
    /* A clip mute already reaches far into the future; play_end must not cut it. */
    int64_t deep = 5000000;
    int64_t m = mic_gate_update(deep, "{\"type\":\"play_end\"}", 1000, TAIL);
    TEST_ASSERT_EQUAL_INT64(deep, m);
}

void test_other_frame_leaves_deadline_unchanged(void) {
    TEST_ASSERT_EQUAL_INT64(42, mic_gate_update(42, "{\"type\":\"welcome\"}", 1000, TAIL));
}

void test_null_frame_is_safe(void) {
    TEST_ASSERT_EQUAL_INT64(7, mic_gate_update(7, NULL, 1000, TAIL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_idle_is_not_muted);
    RUN_TEST(test_play_begin_with_ms_mutes_for_clip_plus_tail);
    RUN_TEST(test_play_begin_without_ms_holds_until_end);
    RUN_TEST(test_play_end_releases_an_unknown_length_hold);
    RUN_TEST(test_play_end_does_not_shorten_a_duration_mute);
    RUN_TEST(test_other_frame_leaves_deadline_unchanged);
    RUN_TEST(test_null_frame_is_safe);
    return UNITY_END();
}
