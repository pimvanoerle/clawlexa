#include "unity.h"
#include "audio_tone.h"

/* Pure sine-buffer generation for the speaker test tone (AU-1, tier T2). */

void setUp(void) {}
void tearDown(void) {}

void test_starts_at_zero(void) {
    int16_t buf[64];
    audio_fill_sine(buf, 64, 1000, 16000, 10000, 0);
    TEST_ASSERT_INT16_WITHIN(50, 0, buf[0]);  /* sin(0) == 0 */
}

void test_stays_within_amplitude(void) {
    int16_t buf[512];
    audio_fill_sine(buf, 512, 440, 16000, 10000, 0);
    for (int i = 0; i < 512; i++) {
        TEST_ASSERT_TRUE(buf[i] >= -10000 && buf[i] <= 10000);
    }
}

void test_quarter_period_hits_peaks(void) {
    /* freq 4000 @ 16000 -> 4 samples/period: 0, +peak, 0, -peak */
    int16_t buf[4];
    audio_fill_sine(buf, 4, 4000, 16000, 10000, 0);
    TEST_ASSERT_INT16_WITHIN(50, 0, buf[0]);
    TEST_ASSERT_INT16_WITHIN(50, 10000, buf[1]);
    TEST_ASSERT_INT16_WITHIN(50, 0, buf[2]);
    TEST_ASSERT_INT16_WITHIN(50, -10000, buf[3]);
}

void test_phase_continuous_across_buffers(void) {
    /* A second buffer starting at sample 128 must continue the first seamlessly. */
    int16_t a[256], b[64];
    audio_fill_sine(a, 256, 1000, 16000, 10000, 0);
    audio_fill_sine(b, 64, 1000, 16000, 10000, 128);
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_INT16(a[128 + i], b[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_at_zero);
    RUN_TEST(test_stays_within_amplitude);
    RUN_TEST(test_quarter_period_hits_peaks);
    RUN_TEST(test_phase_continuous_across_buffers);
    return UNITY_END();
}
