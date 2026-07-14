#include "unity.h"
#include "wake_gate.h"

/* Wake-gate transitions (Phase 4, tier T2). LISTENING <-> STREAMING. */

void setUp(void) {}
void tearDown(void) {}

void test_wake_starts_a_turn(void) {
    TEST_ASSERT_EQUAL(WAKE_STREAMING, wake_gate_next(WAKE_LISTENING, WAKE_EV_WAKE));
}

void test_turn_end_while_listening_is_noop(void) {
    TEST_ASSERT_EQUAL(WAKE_LISTENING, wake_gate_next(WAKE_LISTENING, WAKE_EV_TURN_END));
}

void test_turn_end_returns_to_listening(void) {
    TEST_ASSERT_EQUAL(WAKE_LISTENING, wake_gate_next(WAKE_STREAMING, WAKE_EV_TURN_END));
}

void test_wake_during_turn_stays_streaming(void) {
    TEST_ASSERT_EQUAL(WAKE_STREAMING, wake_gate_next(WAKE_STREAMING, WAKE_EV_WAKE));
}

/* Phase 6b link-error state: LINK_DOWN forces ERROR from anywhere; LINK_UP
 * re-arms LISTENING; ERROR is sticky until the link returns. */

void test_link_down_from_listening_enters_error(void) {
    TEST_ASSERT_EQUAL(WAKE_ERROR, wake_gate_next(WAKE_LISTENING, WAKE_EV_LINK_DOWN));
}

void test_link_down_from_streaming_enters_error(void) {
    TEST_ASSERT_EQUAL(WAKE_ERROR, wake_gate_next(WAKE_STREAMING, WAKE_EV_LINK_DOWN));
}

void test_error_ignores_wake_and_turn_end(void) {
    TEST_ASSERT_EQUAL(WAKE_ERROR, wake_gate_next(WAKE_ERROR, WAKE_EV_WAKE));
    TEST_ASSERT_EQUAL(WAKE_ERROR, wake_gate_next(WAKE_ERROR, WAKE_EV_TURN_END));
}

void test_link_up_recovers_to_listening(void) {
    TEST_ASSERT_EQUAL(WAKE_LISTENING, wake_gate_next(WAKE_ERROR, WAKE_EV_LINK_UP));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_wake_starts_a_turn);
    RUN_TEST(test_turn_end_while_listening_is_noop);
    RUN_TEST(test_turn_end_returns_to_listening);
    RUN_TEST(test_wake_during_turn_stays_streaming);
    RUN_TEST(test_link_down_from_listening_enters_error);
    RUN_TEST(test_link_down_from_streaming_enters_error);
    RUN_TEST(test_error_ignores_wake_and_turn_end);
    RUN_TEST(test_link_up_recovers_to_listening);
    return UNITY_END();
}
