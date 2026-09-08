#pragma once

#include <stdbool.h>

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
 * A third state, ERROR, is entered from anywhere when the link to the bridge
 * goes down (WAKE_EV_LINK_DOWN): the device shows the error crab and streams
 * nothing. WAKE_EV_LINK_UP returns it to LISTENING when the link recovers. The
 * debounce and the tap-to-restart affordance live in the IO layer, not here.
 *
 * This is only the transition logic; the wake detector and the start/stop-
 * streaming IO are the swappable edges that drive and act on it. */

typedef enum {
    WAKE_LISTENING,   /* armed for the wake word; not streaming */
    WAKE_STREAMING,   /* a wake-triggered conversation is streaming to the bridge */
    WAKE_ERROR,       /* link to the bridge is down; error crab, tap to restart */
} wake_state_t;

typedef enum {
    WAKE_EV_WAKE,       /* wake word detected */
    WAKE_EV_TURN_END,   /* conversation finished: bridge end_turn / tap / link lost */
    WAKE_EV_LINK_DOWN,  /* the link to the bridge went down */
    WAKE_EV_LINK_UP,    /* the link to the bridge came back */
} wake_event_t;

/* The next state given the current state and an event. Pure / no IO. */
wake_state_t wake_gate_next(wake_state_t state, wake_event_t event);

/* What opens a conversation from LISTENING on a given tick (pure, host-tested).
 *
 * Three things can open one: the wake word, a screen tap (push-to-talk), and —
 * since Phase 6c — the bridge, via a `start_turn` control frame (SPEC §7a: the
 * `listen()` tool, used by the presence greeting). They differ in what happens
 * when the mic is muted because our own reply is still playing:
 *
 *   - a tap is *dropped* (the mic is deaf anyway, and the user is most likely
 *     reacting to what they just heard);
 *   - a remote start_turn is *queued* — it fires as soon as the mute clears.
 *     The bridge typically sends it right after a clip it just spoke ("hi, I'm
 *     listening"), so it lands squarely in the mute tail; dropping it there
 *     would mean the greeting opens a window that never opens.
 *
 * The caller passes the pending flags rather than consuming them, and clears
 * exactly the ones `consume_*` names — that's what keeps the queueing decision
 * here, in a testable function, instead of in the mic task's control flow. */
typedef struct {
    bool open;            /* start streaming a conversation this tick */
    bool consume_tap;     /* clear the pending-tap flag */
    bool consume_remote;  /* clear the pending remote-wake flag */
} wake_trigger_t;

wake_trigger_t wake_trigger_eval(bool muted, bool wake_fired,
                                 bool tap_pending, bool remote_pending);
