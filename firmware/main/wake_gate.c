#include "wake_gate.h"

wake_state_t wake_gate_next(wake_state_t state, wake_event_t event) {
    switch (state) {
    case WAKE_LISTENING:
        /* Only the wake word starts a turn; a stray TURN_END is a no-op. */
        return event == WAKE_EV_WAKE ? WAKE_STREAMING : WAKE_LISTENING;
    case WAKE_STREAMING:
        /* The turn ends on TURN_END; a re-fired wake mid-turn is ignored. */
        return event == WAKE_EV_TURN_END ? WAKE_LISTENING : WAKE_STREAMING;
    }
    return WAKE_LISTENING;  /* unreachable; safe default */
}
