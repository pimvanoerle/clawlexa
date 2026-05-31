#include <string.h>

#include "unity.h"
#include "wav.h"

/* Pure WAV-header parsing for the embedded boot sound (1c-a, tier T2). */

void setUp(void) {}
void tearDown(void) {}

/* Build a minimal 16 kHz / mono / 16-bit WAV with `data_bytes` of payload. */
static size_t make_wav(uint8_t *buf, uint16_t ch, uint32_t rate, uint16_t bits,
                       uint32_t data_bytes) {
    size_t p = 0;
    memcpy(buf + p, "RIFF", 4); p += 4;
    uint32_t riff = 36 + data_bytes;
    buf[p++] = riff; buf[p++] = riff >> 8; buf[p++] = riff >> 16; buf[p++] = riff >> 24;
    memcpy(buf + p, "WAVE", 4); p += 4;
    memcpy(buf + p, "fmt ", 4); p += 4;
    uint32_t fmtlen = 16;
    buf[p++] = fmtlen; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 1; buf[p++] = 0;                       /* PCM */
    buf[p++] = ch; buf[p++] = ch >> 8;
    buf[p++] = rate; buf[p++] = rate >> 8; buf[p++] = rate >> 16; buf[p++] = rate >> 24;
    uint32_t byterate = rate * ch * bits / 8;
    buf[p++] = byterate; buf[p++] = byterate >> 8; buf[p++] = byterate >> 16; buf[p++] = byterate >> 24;
    uint16_t blockalign = ch * bits / 8;
    buf[p++] = blockalign; buf[p++] = blockalign >> 8;
    buf[p++] = bits; buf[p++] = bits >> 8;
    memcpy(buf + p, "data", 4); p += 4;
    buf[p++] = data_bytes; buf[p++] = data_bytes >> 8;
    buf[p++] = data_bytes >> 16; buf[p++] = data_bytes >> 24;
    for (uint32_t i = 0; i < data_bytes; i++) {
        buf[p++] = (uint8_t)i;
    }
    return p;
}

void test_parses_canonical_wav(void) {
    uint8_t buf[128];
    size_t n = make_wav(buf, 1, 16000, 16, 8);
    wav_info_t info;
    TEST_ASSERT_TRUE(wav_parse(buf, n, &info));
    TEST_ASSERT_EQUAL_UINT32(16000, info.sample_rate);
    TEST_ASSERT_EQUAL_UINT16(1, info.channels);
    TEST_ASSERT_EQUAL_UINT16(16, info.bits_per_sample);
    TEST_ASSERT_EQUAL_UINT32(8, info.data_bytes);
    TEST_ASSERT_EQUAL_UINT8(0, info.data[0]);
    TEST_ASSERT_EQUAL_UINT8(7, info.data[7]);
}

void test_rejects_non_riff(void) {
    uint8_t buf[64];
    size_t n = make_wav(buf, 1, 16000, 16, 4);
    memcpy(buf, "JUNK", 4);
    wav_info_t info;
    TEST_ASSERT_FALSE(wav_parse(buf, n, &info));
}

void test_rejects_truncated(void) {
    uint8_t buf[128];
    size_t n = make_wav(buf, 1, 16000, 16, 32);
    wav_info_t info;
    /* claims 32 data bytes but we pass a length cut short of them */
    TEST_ASSERT_FALSE(wav_parse(buf, n - 16, &info));
}

void test_rejects_too_short(void) {
    uint8_t buf[8] = {'R', 'I', 'F', 'F', 0, 0, 0, 0};
    wav_info_t info;
    TEST_ASSERT_FALSE(wav_parse(buf, sizeof(buf), &info));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_canonical_wav);
    RUN_TEST(test_rejects_non_riff);
    RUN_TEST(test_rejects_truncated);
    RUN_TEST(test_rejects_too_short);
    return UNITY_END();
}
