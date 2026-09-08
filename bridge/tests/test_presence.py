"""Unit tests for the presence greeting policy (SPEC §7a, Phase 6c).

Pure logic with both clocks injected — no sensor, no Home Assistant, no waiting.
"""
from clawlexa_bridge.presence import (DEFAULT_GREETINGS, GreetingPolicy,
                                      greeting_for, in_quiet_hours, time_of_day)

MIN = 60.0


class Clock:
    def __init__(self) -> None:
        self.t = 0.0
        self.hour = 10  # a non-quiet hour by default

    def __call__(self) -> float:
        return self.t

    def advance(self, dt: float) -> None:
        self.t += dt


def make(away_s=30 * MIN, min_gap_s=30 * MIN, quiet=(22, 8)):
    clk = Clock()
    pol = GreetingPolicy(away_s=away_s, min_gap_s=min_gap_s,
                         quiet_start_h=quiet[0], quiet_end_h=quiet[1],
                         now=clk, hour=lambda: clk.hour)
    return pol, clk


def arrive_after(pol, clk, away_s):
    """Leave the room, wait `away_s`, come back. Returns the greet decision."""
    pol.update(False)
    clk.advance(away_s)
    return pol.update(True)


# --- the happy path ---------------------------------------------------------

def test_returning_after_a_long_absence_greets():
    pol, clk = make(away_s=30 * MIN)
    pol.update(True)          # first reading: already in the room
    assert arrive_after(pol, clk, 31 * MIN)


def test_greeting_rotates_and_matches_time_of_day():
    pol, clk = make()
    pol.update(True)
    assert arrive_after(pol, clk, 31 * MIN)
    first = pol.greeting()
    assert first == DEFAULT_GREETINGS["morning"][0]
    assert arrive_after(pol, clk, 31 * MIN)
    assert pol.greeting() == DEFAULT_GREETINGS["morning"][1] != first


# --- the suppression rules --------------------------------------------------

def test_first_reading_never_greets():
    """At startup we don't know where you've been — don't greet whoever is
    already sitting there."""
    pol, clk = make()
    assert not pol.update(True)


def test_coffee_run_does_not_greet():
    pol, clk = make(away_s=30 * MIN)
    pol.update(True)
    assert not arrive_after(pol, clk, 5 * MIN)


def test_repeated_occupied_readings_are_not_an_edge():
    """A sensor that re-reports 'occupied' while you sit there greets once, never
    again."""
    pol, clk = make()
    pol.update(True)
    assert arrive_after(pol, clk, 31 * MIN)
    for _ in range(5):
        clk.advance(MIN)
        assert not pol.update(True)


def test_quiet_hours_suppress_the_greeting():
    pol, clk = make(quiet=(22, 8))
    clk.hour = 23
    pol.update(True)
    assert not arrive_after(pol, clk, 31 * MIN)


def test_greeting_resumes_after_quiet_hours():
    pol, clk = make(quiet=(22, 8))
    clk.hour = 2
    pol.update(True)
    assert not arrive_after(pol, clk, 31 * MIN)
    clk.hour = 9
    assert arrive_after(pol, clk, 31 * MIN)


def test_busy_conversation_suppresses_the_greeting():
    """Don't talk over a conversation that's already running."""
    pol, clk = make()
    pol.update(True)
    pol.update(False)
    clk.advance(31 * MIN)
    assert not pol.update(True, busy=True)


def test_min_gap_debounces_a_flapping_sensor():
    """Even with qualifying absences, two greetings never land inside min_gap."""
    pol, clk = make(away_s=10 * MIN, min_gap_s=60 * MIN)
    pol.update(True)
    assert arrive_after(pol, clk, 11 * MIN)      # greeted at t=11min
    assert not arrive_after(pol, clk, 11 * MIN)  # t=22min: only 11 min on from it
    assert not arrive_after(pol, clk, 40 * MIN)  # t=62min: still inside the gap
    assert arrive_after(pol, clk, 11 * MIN)      # t=73min: 62 min on — greets


def test_absence_is_measured_from_the_last_clear():
    """Time already spent in the room doesn't count toward the absence."""
    pol, clk = make(away_s=30 * MIN)
    pol.update(True)
    clk.advance(2 * 60 * MIN)  # a long stint at the desk
    assert not arrive_after(pol, clk, 5 * MIN)


# --- the small pure helpers -------------------------------------------------

def test_time_of_day_buckets():
    assert time_of_day(0) == time_of_day(11) == "morning"
    assert time_of_day(12) == time_of_day(17) == "afternoon"
    assert time_of_day(18) == time_of_day(23) == "evening"


def test_quiet_hours_window_wraps_midnight():
    assert in_quiet_hours(23, 22, 8) and in_quiet_hours(3, 22, 8)
    assert not in_quiet_hours(9, 22, 8) and not in_quiet_hours(21, 22, 8)


def test_quiet_hours_same_day_window_and_disabled():
    assert in_quiet_hours(3, 1, 6) and not in_quiet_hours(7, 1, 6)
    assert not in_quiet_hours(h := 3, 0, 0) and not in_quiet_hours(23, 0, 0)


def test_greeting_for_accepts_a_custom_table():
    table = {"morning": ("clack",), "afternoon": ("clack",), "evening": ("snip",)}
    assert greeting_for(9, 0, table) == "clack"
    assert greeting_for(20, 7, table) == "snip"


# --- why a reading was refused --------------------------------------------
# A suppressed arrival is silent otherwise, and indistinguishable from a sensor
# that never fired — which cost a whole round-trip to the study to work out.

def test_reason_explains_a_too_short_absence():
    pol, clk = make(away_s=30 * MIN)
    pol.update(True)
    assert not arrive_after(pol, clk, 5 * MIN)
    assert "only away" in pol.reason and "300s" in pol.reason


def test_reason_explains_the_non_edge_and_first_reading_cases():
    pol, clk = make()
    pol.update(True)
    assert "first reading" in pol.reason
    clk.advance(MIN)
    pol.update(True)
    assert "already occupied" in pol.reason


def test_reason_explains_quiet_hours_and_busy():
    pol, clk = make(quiet=(22, 8))
    clk.hour = 23
    pol.update(True)
    assert not arrive_after(pol, clk, 31 * MIN)
    assert "quiet hours" in pol.reason

    pol2, clk2 = make()
    pol2.update(True)
    pol2.update(False)
    clk2.advance(31 * MIN)
    assert not pol2.update(True, busy=True)
    assert "already live" in pol2.reason


def test_reason_reports_a_successful_greeting():
    pol, clk = make(away_s=30 * MIN)
    pol.update(True)
    assert arrive_after(pol, clk, 45 * MIN)
    assert "greeting" in pol.reason and "45 min" in pol.reason
