#include "unity.h"
#include "mic_sample.h"

/* ICS-43434 raw-32 -> PCM16 conversion (1c-b, tier T2). Shift is 13. */

void setUp(void) {}
void tearDown(void) {}

void test_zero_is_zero(void) {
    TEST_ASSERT_EQUAL_INT16(0, mic_sample_to_pcm16(0));
}

void test_midscale_round_trips(void) {
    /* A value that stays in range: (100 << 13) >> 13 == 100. */
    TEST_ASSERT_EQUAL_INT16(100, mic_sample_to_pcm16(100 << 13));
    TEST_ASSERT_EQUAL_INT16(-100, mic_sample_to_pcm16(-(100 << 13)));
}

void test_clamps_high(void) {
    TEST_ASSERT_EQUAL_INT16(32767, mic_sample_to_pcm16(0x7FFFFFFF));
}

void test_clamps_low(void) {
    TEST_ASSERT_EQUAL_INT16(-32768, mic_sample_to_pcm16(INT32_MIN));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_zero_is_zero);
    RUN_TEST(test_midscale_round_trips);
    RUN_TEST(test_clamps_high);
    RUN_TEST(test_clamps_low);
    return UNITY_END();
}
