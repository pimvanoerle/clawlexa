#include "unity.h"
#include "pcm_carry.h"
#include <string.h>

/* Reassembling PCM from arbitrarily-chunked bytes (tier T2). This is the
 * intermittent-crackle bug: the WebSocket client delivered a 1024-byte frame as
 * 209 + 815 bytes, and taking len/2 from each dropped a byte and byte-shifted
 * everything after it. */

static pcm_carry_t c;
static int16_t out[64];

void setUp(void) { pcm_carry_reset(&c); memset(out, 0, sizeof(out)); }
void tearDown(void) {}

void test_even_chunk_needs_no_carry(void) {
    const uint8_t in[4] = {0x01, 0x02, 0x03, 0x04};
    size_t used = 0;
    size_t n = pcm_carry_feed(&c, in, 4, out, 64, &used);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT(4, used);
    TEST_ASSERT_FALSE(c.pending);
    TEST_ASSERT_EQUAL_INT16(0x0201, out[0]);   /* little-endian */
    TEST_ASSERT_EQUAL_INT16(0x0403, out[1]);
}

void test_odd_chunk_carries_its_last_byte(void) {
    const uint8_t in[3] = {0x01, 0x02, 0xAA};
    size_t used = 0;
    size_t n = pcm_carry_feed(&c, in, 3, out, 64, &used);
    TEST_ASSERT_EQUAL_UINT(1, n);              /* one whole sample */
    TEST_ASSERT_EQUAL_UINT(3, used);           /* all three bytes accounted for */
    TEST_ASSERT_TRUE(c.pending);
    TEST_ASSERT_EQUAL_UINT8(0xAA, c.byte);     /* held, not dropped */
}

void test_carried_byte_pairs_with_the_next_chunk(void) {
    const uint8_t first[3] = {0x01, 0x02, 0xAA};
    const uint8_t second[3] = {0xBB, 0x03, 0x04};
    size_t used = 0;
    pcm_carry_feed(&c, first, 3, out, 64, &used);
    size_t n = pcm_carry_feed(&c, second, 3, out, 64, &used);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_INT16((int16_t)0xBBAA, out[0]);  /* the split sample */
    TEST_ASSERT_EQUAL_INT16(0x0403, out[1]);
    TEST_ASSERT_FALSE(c.pending);
}

void test_a_split_stream_matches_the_unsplit_one(void) {
    /* The actual regression: the same bytes, chunked oddly, must produce
     * identical samples to one clean pass. */
    uint8_t stream[32];
    for (int i = 0; i < 32; i++) stream[i] = (uint8_t)(i + 1);

    int16_t whole[16];
    size_t used = 0;
    size_t n_whole = pcm_carry_feed(&c, stream, 32, whole, 16, &used);

    pcm_carry_reset(&c);
    int16_t split[16];
    size_t total = 0, pos = 0;
    const size_t chunks[] = {5, 1, 7, 3, 16};   /* deliberately odd sizes */
    for (size_t i = 0; i < 5; i++) {
        size_t u = 0;
        total += pcm_carry_feed(&c, stream + pos, chunks[i],
                                split + total, 16 - total, &u);
        pos += chunks[i];
    }
    TEST_ASSERT_EQUAL_UINT(n_whole, total);
    TEST_ASSERT_EQUAL_INT16_ARRAY(whole, split, (int) n_whole);
}

void test_single_byte_chunks_still_reassemble(void) {
    /* The pathological case the old `len >= 2` guard silently discarded. */
    const uint8_t stream[4] = {0x11, 0x22, 0x33, 0x44};
    size_t total = 0;
    for (int i = 0; i < 4; i++) {
        size_t u = 0;
        total += pcm_carry_feed(&c, &stream[i], 1, out + total, 64 - total, &u);
    }
    TEST_ASSERT_EQUAL_UINT(2, total);
    TEST_ASSERT_EQUAL_INT16(0x2211, out[0]);
    TEST_ASSERT_EQUAL_INT16(0x4433, out[1]);
}

void test_output_capacity_is_respected_and_the_rest_reported(void) {
    const uint8_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    size_t used = 0;
    size_t n = pcm_carry_feed(&c, in, 8, out, 2, &used);   /* room for 2 only */
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT(4, used);        /* caller re-feeds from byte 4 */
    size_t n2 = pcm_carry_feed(&c, in + used, 8 - used, out + n, 62, &used);
    TEST_ASSERT_EQUAL_UINT(2, n2);
}

void test_reset_drops_a_pending_byte(void) {
    const uint8_t in[1] = {0xAA};
    size_t used = 0;
    pcm_carry_feed(&c, in, 1, out, 64, &used);
    TEST_ASSERT_TRUE(c.pending);
    pcm_carry_reset(&c);                    /* new clip: don't leak across it */
    TEST_ASSERT_FALSE(c.pending);
}

void test_empty_chunk_is_harmless(void) {
    size_t used = 99;
    TEST_ASSERT_EQUAL_UINT(0, pcm_carry_feed(&c, (const uint8_t *) "", 0, out, 64, &used));
    TEST_ASSERT_EQUAL_UINT(0, used);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_even_chunk_needs_no_carry);
    RUN_TEST(test_odd_chunk_carries_its_last_byte);
    RUN_TEST(test_carried_byte_pairs_with_the_next_chunk);
    RUN_TEST(test_a_split_stream_matches_the_unsplit_one);
    RUN_TEST(test_single_byte_chunks_still_reassemble);
    RUN_TEST(test_output_capacity_is_respected_and_the_rest_reported);
    RUN_TEST(test_reset_drops_a_pending_byte);
    RUN_TEST(test_empty_chunk_is_harmless);
    return UNITY_END();
}
