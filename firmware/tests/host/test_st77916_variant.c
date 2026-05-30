#include "unity.h"
#include "st77916_variant.h"

/* The two ST77916 panels on the 1.85C are distinguished by register 0x04.
 * This is the decision core of the display bring-up footgun (task_ideas DT-3),
 * extracted so it can be graded with no hardware. */

void setUp(void) {}
void tearDown(void) {}

void test_new_variant_id(void) {
    const uint8_t id[4] = {0x00, 0x02, 0x7F, 0x7F};
    TEST_ASSERT_EQUAL(ST77916_VARIANT_NEW, st77916_variant_from_id(id));
}

void test_default_variant_id(void) {
    const uint8_t id[4] = {0x00, 0x7F, 0x7F, 0x7F};
    TEST_ASSERT_EQUAL(ST77916_VARIANT_DEFAULT, st77916_variant_from_id(id));
}

void test_unknown_id_falls_back_to_default(void) {
    const uint8_t id[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL(ST77916_VARIANT_DEFAULT, st77916_variant_from_id(id));
}

void test_failed_read_zeroed_id_is_default(void) {
    /* A failed register read leaves the id buffer zeroed; must not pick NEW. */
    const uint8_t id[4] = {0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL(ST77916_VARIANT_DEFAULT, st77916_variant_from_id(id));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_new_variant_id);
    RUN_TEST(test_default_variant_id);
    RUN_TEST(test_unknown_id_falls_back_to_default);
    RUN_TEST(test_failed_read_zeroed_id_is_default);
    return UNITY_END();
}
