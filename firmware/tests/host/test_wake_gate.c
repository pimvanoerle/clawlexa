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

/* Phase 6c: what opens a conversation on a tick — wake word, tap, or the
 * bridge's start_turn — and which of those survive the half-duplex mute. */

void test_nothing_pending_opens_nothing(void) {
    wake_trigger_t t = wake_trigger_eval(false, false, false, false);
    TEST_ASSERT_FALSE(t.open);
    TEST_ASSERT_FALSE(t.consume_tap);
    TEST_ASSERT_FALSE(t.consume_remote);
}

void test_wake_word_opens_a_conversation(void) {
    TEST_ASSERT_TRUE(wake_trigger_eval(false, true, false, false).open);
}

void test_tap_opens_a_conversation_and_is_consumed(void) {
    wake_trigger_t t = wake_trigger_eval(false, false, true, false);
    TEST_ASSERT_TRUE(t.open);
    TEST_ASSERT_TRUE(t.consume_tap);
}

void test_remote_start_turn_opens_a_conversation_and_is_consumed(void) {
    wake_trigger_t t = wake_trigger_eval(false, false, false, true);
    TEST_ASSERT_TRUE(t.open);
    TEST_ASSERT_TRUE(t.consume_remote);
}

void test_muted_drops_a_tap(void) {
    wake_trigger_t t = wake_trigger_eval(true, false, true, false);
    TEST_ASSERT_FALSE(t.open);
    TEST_ASSERT_TRUE(t.consume_tap);  /* dropped, not queued */
}

void test_muted_queues_a_remote_start_turn(void) {
    /* The greeting case: the bridge sends start_turn right after the clip it
     * spoke, so it lands in the mute tail. It must survive to the next tick. */
    wake_trigger_t t = wake_trigger_eval(true, false, false, true);
    TEST_ASSERT_FALSE(t.open);
    TEST_ASSERT_FALSE(t.consume_remote);
    /* mute clears, flag still pending -> now it opens the window */
    wake_trigger_t after = wake_trigger_eval(false, false, false, true);
    TEST_ASSERT_TRUE(after.open);
    TEST_ASSERT_TRUE(after.consume_remote);
}

void test_muted_never_opens_even_on_a_wake(void) {
    TEST_ASSERT_FALSE(wake_trigger_eval(true, true, true, true).open);
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
    RUN_TEST(test_nothing_pending_opens_nothing);
    RUN_TEST(test_wake_word_opens_a_conversation);
    RUN_TEST(test_tap_opens_a_conversation_and_is_consumed);
    RUN_TEST(test_remote_start_turn_opens_a_conversation_and_is_consumed);
    RUN_TEST(test_muted_drops_a_tap);
    RUN_TEST(test_muted_queues_a_remote_start_turn);
    RUN_TEST(test_muted_never_opens_even_on_a_wake);
    return UNITY_END();
}
