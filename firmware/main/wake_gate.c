#include "wake_gate.h"

wake_state_t wake_gate_next(wake_state_t state, wake_event_t event) {
    /* A lost link overrides any state; a restored link re-arms LISTENING. */
    if (event == WAKE_EV_LINK_DOWN) {
        return WAKE_ERROR;
    }
    if (event == WAKE_EV_LINK_UP) {
        return WAKE_LISTENING;
    }
    switch (state) {
    case WAKE_LISTENING:
        /* Only the wake word starts a turn; a stray TURN_END is a no-op. */
        return event == WAKE_EV_WAKE ? WAKE_STREAMING : WAKE_LISTENING;
    case WAKE_STREAMING:
        /* The turn ends on TURN_END; a re-fired wake mid-turn is ignored. */
        return event == WAKE_EV_TURN_END ? WAKE_LISTENING : WAKE_STREAMING;
    case WAKE_ERROR:
        /* Only WAKE_EV_LINK_UP (handled above) leaves ERROR. */
        return WAKE_ERROR;
    }
    return WAKE_LISTENING;  /* unreachable; safe default */
}

wake_trigger_t wake_trigger_eval(bool muted, bool wake_fired,
                                 bool tap_pending, bool remote_pending) {
    wake_trigger_t t = {false, false, false};
    if (muted) {
        /* Our own reply is still draining from the speaker: nothing opens a
         * conversation yet. Drop a tap; hold the remote start_turn for the tick
         * after the mute clears (see wake_gate.h). */
        t.consume_tap = tap_pending;
        return t;
    }
    t.open = wake_fired || tap_pending || remote_pending;
    t.consume_tap = tap_pending;
    t.consume_remote = remote_pending;
    return t;
}
