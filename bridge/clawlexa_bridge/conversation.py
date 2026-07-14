"""Bridge-driven conversation window (SPEC §7, Phase 6b).

A wake opens a *conversation*, not a single turn: the device keeps streaming its
mic and the bridge decides when the conversation has gone idle, then tells the
device to stop (an `end_turn` control frame) and re-arm its wake word. Putting
this timing on the bridge — which already runs the VAD and sees the agent's
replies — keeps the firmware dumb and makes the logic host-testable.

`Conversation` is a pure state machine: feed it the events the server observes
(wake, user voice activity, an utterance handed to the agent, a reply starting
and finishing, the stream closing) and poll `should_end()`. It owns no IO and
takes an injectable clock, so tests drive it with a fake `now`.

Two timers, so the window never fires at the wrong moment:
  - **follow-up window** — after a reply finishes (or a bare wake with no
    speech), how long of *real silence* to wait before re-arming. User voice
    activity keeps pushing this out, so it only ever counts silence.
  - **reply timeout** — while the agent owes a reply (thinking or speaking) the
    follow-up window is suspended so a slow first-turn cold start can't trip a
    re-arm; a longer reply timeout is the only thing that ends the conversation
    then, covering an agent that never answers.
"""
from __future__ import annotations

import time
from typing import Callable

# Follow-up silence before re-arming the wake word, and the safety cap on how
# long we hold the conversation open waiting for an agent reply. The window is
# generous (natural pauses, and a reply may invite a response) since a "bye" ends
# the conversation immediately anyway — so a longer window doesn't feel like it's
# hanging. The reply cap must sit *above* the agent's own per-turn timeout
# (voice_agent's --brain-timeout, default 120s) so the agent's own reply/error
# path closes the turn (a spoken reply -> play_end) rather than this safety net
# firing mid-think and re-arming the wake word before the reply lands.
DEFAULT_WINDOW_S = 12.0
DEFAULT_REPLY_TIMEOUT_S = 150.0


class Conversation:
    def __init__(self, *, window_s: float = DEFAULT_WINDOW_S,
                 reply_timeout_s: float = DEFAULT_REPLY_TIMEOUT_S,
                 now: Callable[[], float] = time.monotonic) -> None:
        self._window_s = window_s
        self._reply_timeout_s = reply_timeout_s
        self._now = now
        self._open = False           # stream is up (wake fired, not yet ended)
        self._last_activity = 0.0    # last user speech or reply audio
        self._await_deadline = 0.0   # >0 while the agent owes a reply; hard cap
        self._force_end = False      # agent asked to end now (goodbye)

    @property
    def open(self) -> bool:
        return self._open

    # --- events the server feeds in -----------------------------------------
    def opened(self) -> None:
        """Wake fired / stream opened."""
        self._open = True
        self._await_deadline = 0.0
        self._force_end = False
        self._last_activity = self._now()

    def closed(self) -> None:
        """Stream ended (we sent end_turn, or the device stopped/dropped)."""
        self._open = False
        self._await_deadline = 0.0
        self._force_end = False

    def end_now(self) -> None:
        """The agent decided the conversation is over (a goodbye) — end at the
        next check, regardless of the follow-up window or a pending reply."""
        self._force_end = True

    def voice_activity(self) -> None:
        """The user is speaking right now — push the silence window out so it
        only ever counts real silence, never someone mid-sentence."""
        self._last_activity = self._now()

    def utterance_submitted(self) -> None:
        """A transcript was handed to the agent — a reply is now owed."""
        now = self._now()
        self._last_activity = now
        self._await_deadline = now + self._reply_timeout_s

    def reply_started(self) -> None:
        """The agent's reply began playing."""
        self._last_activity = self._now()

    def reply_finished(self, has_more: bool = False) -> None:
        """The agent's reply finished. `has_more` = more utterances are still
        queued for the agent, so keep awaiting the next reply; otherwise start
        the follow-up silence window."""
        now = self._now()
        self._last_activity = now
        self._await_deadline = now + self._reply_timeout_s if has_more else 0.0

    # --- the decision --------------------------------------------------------
    def should_end(self) -> bool:
        """True when the conversation has gone idle and the device should be told
        to stop streaming and re-arm the wake word."""
        if not self._open:
            return False
        if self._force_end:  # agent said goodbye — end immediately
            return True
        now = self._now()
        if self._await_deadline:
            # A reply is owed (thinking/speaking): hold open until the safety cap.
            return now >= self._await_deadline
        return (now - self._last_activity) >= self._window_s
