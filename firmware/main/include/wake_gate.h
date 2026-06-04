#pragma once

/* Wake-gate state machine (pure, host-tested) — the core of Phase 4.
 *
 * The device idles in LISTENING: the wake detector is armed and the mic is NOT
 * streamed to the bridge. When the wake word fires, it moves to STREAMING (the
 * mic streams one turn). The turn ends — back to LISTENING — when the bridge's
 * spoken reply completes (play_end) or a no-reply timeout elapses. v1 is
 * half-duplex, single-turn-per-wake (barge-in / multi-turn come later).
 *
 * This is only the transition logic; the wake detector (Porcupine) and the
 * start/stop-streaming IO are the swappable edges that drive and act on it. */

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
