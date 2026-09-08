"""Presence-driven greetings (SPEC §7a, Phase 6c).

An *ambient trigger*: the room starts the conversation instead of the user. A
presence sensor says the study just became occupied, and — if the user has been
gone long enough for it to mean something — clawlexa says hi and opens a
listening window.

This module is the pure half: the decision ("is this arrival worth greeting?")
and the words ("morning!"). No sockets, no Home Assistant, no clock of its own —
both clocks are injected, so "stepped out for coffee, came back" is a unit test
rather than a walk to the study. The IO half lives in `ha.py`.
"""
from __future__ import annotations

import time
from typing import Callable, Optional, Sequence

# The room must have been clear this long for a return to count as an *arrival*.
# Long enough that a coffee run doesn't trigger it, short enough that coming
# back after lunch does.
DEFAULT_AWAY_S = 30 * 60
# Never two greetings closer than this, whatever the sensor claims — a flapping
# or double-firing sensor shouldn't produce a chatty crab.
DEFAULT_MIN_GAP_S = 30 * 60
# Quiet hours [start, end) in local time: no greetings overnight.
DEFAULT_QUIET_START_H = 22
DEFAULT_QUIET_END_H = 8

# Canned greetings by time of day. Deliberately generic — the bridge stays
# agent-agnostic (SPEC §2), and a driver with a persona passes its own table.
DEFAULT_GREETINGS: dict[str, tuple[str, ...]] = {
    "morning": (
        "Morning! I'm here if you need me.",
        "Morning. Good to see you.",
        "Hey, morning. Ready when you are.",
    ),
    "afternoon": (
        "Hey, welcome back.",
        "Afternoon! Back at it?",
        "Hello again. I'm listening.",
    ),
    "evening": (
        "Evening! Back for more?",
        "Hey, good evening.",
        "Evening. I'm around if you want me.",
    ),
}


def time_of_day(hour: int) -> str:
    """Bucket a 0-23 local hour into a greeting table key."""
    if hour < 12:
        return "morning"
    if hour < 18:
        return "afternoon"
    return "evening"


def greeting_for(hour: int, n: int,
                 table: Optional[dict[str, Sequence[str]]] = None) -> str:
    """The `n`th greeting for this hour — rotating, not random, so the same
    sequence is reproducible in a test and the crab doesn't repeat itself twice
    in a row in the study."""
    lines = (table or DEFAULT_GREETINGS)[time_of_day(hour)]
    return lines[n % len(lines)]


def in_quiet_hours(hour: int, start_h: int, end_h: int) -> bool:
    """True if `hour` falls in the quiet window [start_h, end_h), which normally
    wraps midnight (22:00-08:00). start == end means 'no quiet hours'."""
    if start_h == end_h:
        return False
    if start_h < end_h:  # a same-day window, e.g. 01:00-06:00
        return start_h <= hour < end_h
    return hour >= start_h or hour < end_h  # wraps midnight


class GreetingPolicy:
    """Decides whether a presence reading is an arrival worth greeting.

    Feed it every reading the sensor produces (`update`); it returns True exactly
    on the edges that deserve a hello. It suppresses:

      - non-edges — the sensor re-reporting "occupied" while you sit there;
      - short absences — you were only clear for `away_s - 1` seconds;
      - the very first reading — at startup we don't know where you've been, and
        greeting whoever is already in the room is the creepy option;
      - quiet hours, and anything within `min_gap_s` of the last greeting;
      - arrivals during a live conversation, which would talk over it.
    """

    def __init__(self, *, away_s: float = DEFAULT_AWAY_S,
                 min_gap_s: float = DEFAULT_MIN_GAP_S,
                 quiet_start_h: int = DEFAULT_QUIET_START_H,
                 quiet_end_h: int = DEFAULT_QUIET_END_H,
                 now: Callable[[], float] = time.monotonic,
                 hour: Callable[[], int] = lambda: time.localtime().tm_hour) -> None:
        self._away_s = away_s
        self._min_gap_s = min_gap_s
        self._quiet = (quiet_start_h, quiet_end_h)
        self._now = now
        self._hour = hour
        self._occupied: Optional[bool] = None  # None = we haven't seen a reading
        self._clear_since: Optional[float] = None
        self._last_greeting: Optional[float] = None
        self._greetings = 0
        self._reason = "no readings yet"

    @property
    def reason(self) -> str:
        """Why the last reading did or didn't earn a greeting, in words.

        Worth its keep: without it a suppressed arrival is *silent*, and telling
        "the sensor never fired" apart from "it fired and I declined" means going
        to Home Assistant's own history to reconstruct the timeline.
        """
        return self._reason

    @property
    def greetings(self) -> int:
        """How many greetings this policy has authorised — also the rotation
        index for `greeting_for`."""
        return self._greetings

    def update(self, occupied: bool, *, busy: bool = False,
               steady_for_s: float = 0.0) -> bool:
        """Feed one presence reading. `busy` = a conversation is already live.

        `steady_for_s` is how long the sensor had *already* been in this state
        when we heard about it — nonzero only for the baseline read at startup,
        where the source knows from Home Assistant's `last_changed`. Without it a
        restart resets the away clock to zero and throws away a real absence: a
        deploy or a crash-restart during a long absence would silently swallow
        the greeting you were owed on your return.

        Returns True if the caller should greet now.
        """
        was = self._occupied
        self._occupied = occupied
        if not occupied:
            if was is not False:  # occupied -> clear (or first-ever reading)
                self._clear_since = self._now() - steady_for_s
                self._reason = (
                    "room went clear; away clock started"
                    if steady_for_s <= 0 else
                    f"room already clear for {steady_for_s / 60:.0f} min; "
                    f"away clock resumed")
            else:
                self._reason = "still clear"
            return False
        if was is None:  # first reading is 'occupied': you were already here
            self._reason = "first reading — can't know how long you'd been here"
            return False
        if was:  # still occupied — not an edge
            self._reason = "already occupied — not an arrival"
            return False
        return self._arrived(busy)

    def _arrived(self, busy: bool) -> bool:
        """A clear -> occupied edge: apply the suppression rules."""
        now = self._now()
        if self._clear_since is None:
            self._reason = "arrival, but we never saw the room go clear"
            return False
        away = now - self._clear_since
        if away < self._away_s:  # only stepped out for a moment
            self._reason = (f"arrival, but only away {away:.0f}s "
                            f"(need {self._away_s:.0f}s)")
            return False
        if busy:
            self._reason = "arrival, but a conversation is already live"
            return False
        if in_quiet_hours(self._hour(), *self._quiet):
            self._reason = f"arrival, but it's quiet hours ({self._hour():02d}:00)"
            return False
        if self._last_greeting is not None and (now - self._last_greeting) < self._min_gap_s:
            self._reason = (f"arrival, but last greeting was only "
                            f"{now - self._last_greeting:.0f}s ago")
            return False
        self._last_greeting = now
        self._greetings += 1
        self._reason = f"arrival after {away / 60:.0f} min away — greeting"
        return True

    def greeting(self, table: Optional[dict[str, Sequence[str]]] = None) -> str:
        """The line to speak for the greeting just authorised."""
        return greeting_for(self._hour(), self._greetings - 1, table)
