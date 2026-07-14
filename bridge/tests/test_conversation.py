"""Unit tests for the bridge-driven conversation window (SPEC §7, Phase 6b).

Pure logic with an injected clock — no sockets, no sleeping.
"""
from clawlexa_bridge.conversation import Conversation


class Clock:
    def __init__(self) -> None:
        self.t = 0.0

    def __call__(self) -> float:
        return self.t

    def advance(self, dt: float) -> None:
        self.t += dt


def make(window_s=7.0, reply_timeout_s=45.0):
    clk = Clock()
    return Conversation(window_s=window_s, reply_timeout_s=reply_timeout_s, now=clk), clk


def test_closed_conversation_never_ends():
    conv, clk = make()
    assert not conv.should_end()  # not opened yet
    clk.advance(100)
    assert not conv.should_end()


def test_bare_wake_then_silence_re_arms_after_window():
    """A wake with no speech re-arms once the follow-up window elapses."""
    conv, clk = make(window_s=7.0)
    conv.opened()
    clk.advance(6.9)
    assert not conv.should_end()
    clk.advance(0.2)  # past 7s of silence
    assert conv.should_end()


def test_window_does_not_fire_while_reply_owed():
    """After the user speaks, a slow (cold-start) agent must not trip a re-arm."""
    conv, clk = make(window_s=7.0, reply_timeout_s=45.0)
    conv.opened()
    conv.utterance_submitted()
    clk.advance(20.0)  # agent still thinking — way past the 7s silence window
    assert not conv.should_end()


def test_reply_finished_restarts_follow_up_window():
    conv, clk = make(window_s=7.0)
    conv.opened()
    conv.utterance_submitted()
    clk.advance(3.0)
    conv.reply_started()
    conv.reply_finished()          # reply done -> follow-up window starts now
    clk.advance(6.9)
    assert not conv.should_end()
    clk.advance(0.2)
    assert conv.should_end()


def test_voice_activity_keeps_window_open():
    """Talking mid-window pushes the silence timer out so we never cut someone off."""
    conv, clk = make(window_s=7.0)
    conv.opened()
    conv.reply_finished()
    clk.advance(6.0)
    conv.voice_activity()          # user starts a follow-up
    clk.advance(6.0)               # 12s since reply, but only 6s since they spoke
    assert not conv.should_end()
    conv.voice_activity()
    clk.advance(7.1)               # now silent past the window
    assert conv.should_end()


def test_reply_timeout_is_the_safety_net():
    """If the agent never replies, the conversation still ends — after the cap."""
    conv, clk = make(window_s=7.0, reply_timeout_s=45.0)
    conv.opened()
    conv.utterance_submitted()
    clk.advance(44.0)
    assert not conv.should_end()
    clk.advance(1.5)               # past the 45s reply timeout
    assert conv.should_end()


def test_has_more_keeps_awaiting_next_reply():
    """A reply finishing with more queued utterances keeps the window suspended."""
    conv, clk = make(window_s=7.0)
    conv.opened()
    conv.utterance_submitted()
    conv.reply_finished(has_more=True)  # another utterance is still queued
    clk.advance(8.0)                    # past the silence window...
    assert not conv.should_end()        # ...but a reply is still owed
    conv.reply_finished(has_more=False)  # queue drained
    clk.advance(7.1)
    assert conv.should_end()


def test_end_now_ends_immediately_even_mid_reply():
    """A goodbye ends the conversation right away, not on a timer."""
    conv, clk = make(window_s=7.0, reply_timeout_s=150.0)
    conv.opened()
    conv.utterance_submitted()     # a reply is owed (normally holds open)
    assert not conv.should_end()
    conv.end_now()
    assert conv.should_end()       # forced end overrides the pending-reply hold


def test_opened_clears_a_prior_force_end():
    conv, clk = make()
    conv.opened()
    conv.end_now()
    conv.closed()
    conv.opened()                  # a fresh wake
    assert not conv.should_end()


def test_closed_after_end_stops_further_ends():
    conv, clk = make(window_s=7.0)
    conv.opened()
    clk.advance(8.0)
    assert conv.should_end()
    conv.closed()                  # we sent end_turn
    assert not conv.should_end()
