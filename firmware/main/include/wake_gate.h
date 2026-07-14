#pragma once

/* Wake-gate state machine (pure, host-tested) — the core of Phase 4 / 6b.
 *
 * The device idles in LISTENING: the wake detector is armed and the mic is NOT
 * streamed to the bridge. When the wake word fires, it moves to STREAMING and
 * the mic streams to the bridge. STREAMING now spans a whole *conversation*
 * (multiple turns, no re-wake between them, SPEC §7): it ends — back to
 * LISTENING — when the bridge sends end_turn (it owns the idle timing), a tap
 * cancels, or the link drops. The pure transition logic below is unchanged from
 * the single-turn version; only the trigger for TURN_END moved from a device
 * timer / play_end to the bridge's end_turn. v1 stays half-duplex (barge-in
 * needs echo cancellation — Phase 8).
 *
 * This is only the transition logic; the wake detector and the start/stop-
 * streaming IO are the swappable edges that drive and act on it. */

typedef enum {
    WAKE_LISTENING,   /* armed for the wake word; not streaming */
    WAKE_STREAMING,   /* a wake-triggered turn is streaming to the bridge */
} wake_state_t;

typedef enum {
    WAKE_EV_WAKE,      /* wake word detected */
    WAKE_EV_TURN_END,  /* turn finished: reply played (play_end) or timed out */
} wake_event_t;

/* The next state given the current state and an event. Pure / no IO. */
wake_state_t wake_gate_next(wake_state_t state, wake_event_t event);
