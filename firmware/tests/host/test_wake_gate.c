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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_wake_starts_a_turn);
    RUN_TEST(test_turn_end_while_listening_is_noop);
    RUN_TEST(test_turn_end_returns_to_listening);
    RUN_TEST(test_wake_during_turn_stays_streaming);
    return UNITY_END();
}
