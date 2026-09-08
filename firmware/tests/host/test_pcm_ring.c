#include "unity.h"
#include "pcm_ring.h"

/* PCM ring buffer (Phase 8 audio smoothing, tier T2). Pure — no I2S, no tasks. */

#define CAP 8
static int16_t storage[CAP];
static pcm_ring_t r;

void setUp(void) { pcm_ring_init(&r, storage, CAP); }
void tearDown(void) {}

void test_starts_empty(void) {
    TEST_ASSERT_EQUAL_UINT(0, pcm_ring_level(&r));
    TEST_ASSERT_EQUAL_UINT(CAP - 1, pcm_ring_free(&r));
}

void test_write_then_read_round_trips(void) {
    const int16_t in[4] = {1, -2, 3, -4};
    int16_t out[4] = {0};
    TEST_ASSERT_EQUAL_UINT(4, pcm_ring_write(&r, in, 4));
    TEST_ASSERT_EQUAL_UINT(4, pcm_ring_level(&r));
    TEST_ASSERT_EQUAL_UINT(4, pcm_ring_read(&r, out, 4));
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 4);
    TEST_ASSERT_EQUAL_UINT(0, pcm_ring_level(&r));
}

void test_partial_read_leaves_the_rest_in_order(void) {
    const int16_t in[5] = {10, 20, 30, 40, 50};
    int16_t out[5] = {0};
    pcm_ring_write(&r, in, 5);
    TEST_ASSERT_EQUAL_UINT(2, pcm_ring_read(&r, out, 2));
    TEST_ASSERT_EQUAL_INT16(10, out[0]);
    TEST_ASSERT_EQUAL_INT16(20, out[1]);
    TEST_ASSERT_EQUAL_UINT(3, pcm_ring_read(&r, out, 5));   /* asks 5, gets 3 */
    TEST_ASSERT_EQUAL_INT16(30, out[0]);
    TEST_ASSERT_EQUAL_INT16(50, out[2]);
}

void test_data_survives_wrapping(void) {
    /* Push the indices near the end, then straddle the wrap. */
    const int16_t pad[5] = {9, 9, 9, 9, 9};
    int16_t sink[5], out[4];
    pcm_ring_write(&r, pad, 5);
    pcm_ring_read(&r, sink, 5);
    const int16_t in[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT(4, pcm_ring_write(&r, in, 4));   /* wraps internally */
    TEST_ASSERT_EQUAL_UINT(4, pcm_ring_read(&r, out, 4));
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 4);
}

void test_full_ring_keeps_one_slot_empty(void) {
    const int16_t in[CAP] = {1, 2, 3, 4, 5, 6, 7, 8};
    TEST_ASSERT_EQUAL_UINT(CAP - 1, pcm_ring_write(&r, in, CAP));
    TEST_ASSERT_EQUAL_UINT(CAP - 1, pcm_ring_level(&r));
    TEST_ASSERT_EQUAL_UINT(0, pcm_ring_free(&r));
}

void test_a_full_ring_reports_a_short_write_not_a_drop(void) {
    /* Regression: the ring used to add the unwritten remainder to `dropped`,
     * but the caller retries it — so a healthy clip that simply ran ahead of
     * the speaker logged hundreds of thousands of "dropped" samples while
     * playing every one of them. A short write means "wait", nothing more. */
    const int16_t in[CAP] = {1, 2, 3, 4, 5, 6, 7, 8};
    TEST_ASSERT_EQUAL_UINT(CAP - 1, pcm_ring_write(&r, in, CAP));
    TEST_ASSERT_EQUAL_UINT(0, r.dropped);
    TEST_ASSERT_EQUAL_UINT(0, pcm_ring_write(&r, in, 3));  /* full: takes none */
    TEST_ASSERT_EQUAL_UINT(0, r.dropped);
}

void test_retrying_a_short_write_loses_nothing(void) {
    /* Exactly what audio_play_pcm does: write what fits, drain, write the rest,
     * and every sample still comes out in order. */
    const int16_t in[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    size_t done = pcm_ring_write(&r, in, 10);
    TEST_ASSERT_EQUAL_UINT(CAP - 1, done);
    int16_t out[10];
    TEST_ASSERT_EQUAL_UINT(CAP - 1, pcm_ring_read(&r, out, 10));
    done += pcm_ring_write(&r, in + done, 10 - done);
    TEST_ASSERT_EQUAL_UINT(10, done);
    TEST_ASSERT_EQUAL_UINT(3, pcm_ring_read(&r, out + CAP - 1, 10));
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 10);
    TEST_ASSERT_EQUAL_UINT(0, r.dropped);
}

void test_read_from_empty_returns_nothing(void) {
    int16_t out[4] = {7, 7, 7, 7};
    TEST_ASSERT_EQUAL_UINT(0, pcm_ring_read(&r, out, 4));
    TEST_ASSERT_EQUAL_INT16(7, out[0]);   /* untouched */
}

void test_reset_discards_audio_and_the_drop_count(void) {
    const int16_t in[CAP] = {1, 2, 3, 4, 5, 6, 7, 8};
    pcm_ring_write(&r, in, CAP);
    pcm_ring_reset(&r);
    TEST_ASSERT_EQUAL_UINT(0, pcm_ring_level(&r));
    TEST_ASSERT_EQUAL_UINT(0, r.dropped);
}

void test_streaming_many_chunks_preserves_the_whole_sequence(void) {
    /* The real pattern: 1 KB frames in, small chunks out, indices wrapping many
     * times over a clip. Every sample must come out exactly once, in order. */
    int16_t next_in = 0, expect = 0;
    for (int round = 0; round < 50; round++) {
        int16_t chunk[3];
        for (int i = 0; i < 3; i++) {
            chunk[i] = next_in++;
        }
        TEST_ASSERT_EQUAL_UINT(3, pcm_ring_write(&r, chunk, 3));
        int16_t out[3];
        TEST_ASSERT_EQUAL_UINT(3, pcm_ring_read(&r, out, 3));
        for (int i = 0; i < 3; i++) {
            TEST_ASSERT_EQUAL_INT16(expect++, out[i]);
        }
    }
    TEST_ASSERT_EQUAL_UINT(0, r.dropped);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_write_then_read_round_trips);
    RUN_TEST(test_partial_read_leaves_the_rest_in_order);
    RUN_TEST(test_data_survives_wrapping);
    RUN_TEST(test_full_ring_keeps_one_slot_empty);
    RUN_TEST(test_a_full_ring_reports_a_short_write_not_a_drop);
    RUN_TEST(test_retrying_a_short_write_loses_nothing);
    RUN_TEST(test_read_from_empty_returns_nothing);
    RUN_TEST(test_reset_discards_audio_and_the_drop_count);
    RUN_TEST(test_streaming_many_chunks_preserves_the_whole_sequence);
    return UNITY_END();
}
