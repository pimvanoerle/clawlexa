"""Unit tests for the Hub — the device-link <-> MCP-agent bridge (Phase 5).

No MCP transport, no real device: the agent loop is exercised directly.
"""
import asyncio
import json
import os

from clawlexa_bridge.conversation import Conversation
from clawlexa_bridge.hub import Hub
from clawlexa_bridge.tts import FakeTTS


class FakeWS:
    """A device connection that just records the control frames it's sent."""
    def __init__(self):
        self.sent = []

    async def send(self, frame):
        self.sent.append(frame)


def test_submit_then_next_returns_transcript():
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        await hub.submit_utterance("hello world")
        return await hub.next_utterance(timeout=1)

    assert asyncio.run(run()) == "hello world"


def test_next_utterance_coalesces_backlog():
    """Utterances that piled up while the agent was busy are drained + combined
    into one, so the agent answers recent speech, not a stale backlog."""
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        await hub.submit_utterance("first thing")
        await hub.submit_utterance("second thing")
        await hub.submit_utterance("third")
        return await hub.next_utterance(timeout=1)

    assert asyncio.run(run()) == "first thing second thing third"


def test_next_utterance_times_out():
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        try:
            await hub.next_utterance(timeout=0.05)
            return "no-timeout"
        except asyncio.TimeoutError:
            return "timeout"

    assert asyncio.run(run()) == "timeout"


def test_end_conversation_forces_the_conversation_to_end():
    """The agent's end_conversation flips the attached Conversation to should_end."""
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        conv = Conversation()
        conv.opened()
        hub.attach(FakeWS(), conv)
        await hub.end_conversation()
        return conv.should_end()

    assert asyncio.run(run()) is True


def test_end_conversation_without_a_conversation_is_a_noop():
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        await hub.end_conversation()  # no device/conversation attached — must not raise
        return "ok"

    assert asyncio.run(run()) == "ok"


def test_speak_without_device_raises():
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        try:
            await hub.speak("hi")
            return "no-error"
        except RuntimeError:
            return "raised"

    assert asyncio.run(run()) == "raised"


def test_speak_synthesizes_and_sends_to_device():
    sent = []

    async def fake_send(ws, path):
        sent.append((ws, path))

    async def run():
        hub = Hub(FakeTTS(), send_wav=fake_send)
        hub.attach("WS")
        assert hub.device_connected
        await hub.speak("it is sunny")

    asyncio.run(run())
    assert len(sent) == 1
    assert sent[0][0] == "WS"
    assert sent[0][1].endswith(".wav") and os.path.exists(sent[0][1])


def test_detach_only_clears_matching_ws():
    hub = Hub(FakeTTS(), send_wav=None)
    hub.attach("WS")
    assert hub.device_connected
    hub.detach("OTHER")          # a different (stale) connection — no-op
    assert hub.device_connected
    hub.detach("WS")
    assert not hub.device_connected


def test_set_state_sends_frame_to_device():
    ws = FakeWS()

    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        hub.attach(ws)
        await hub.set_state("listening")

    asyncio.run(run())
    assert json.loads(ws.sent[0]) == {"type": "set_state", "state": "listening"}


def test_show_sends_frame_to_device():
    ws = FakeWS()

    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        hub.attach(ws)
        await hub.show("on my way")

    asyncio.run(run())
    assert json.loads(ws.sent[0]) == {"type": "show", "text": "on my way"}


def test_set_state_without_device_raises():
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        try:
            await hub.set_state("idle")
            return "no-error"
        except RuntimeError:
            return "raised"

    assert asyncio.run(run()) == "raised"


def test_listen_sends_start_turn_to_the_device():
    """`listen()` opens a window with no wake word (SPEC §7a) — the device does
    the rest, so all the Hub owes is the frame."""
    async def run():
        ws = FakeWS()
        hub = Hub(FakeTTS(), send_wav=None)
        hub.attach(ws, Conversation())
        await hub.listen()
        return ws.sent

    sent = asyncio.run(run())
    assert [json.loads(f)["type"] for f in sent] == ["start_turn"]


def test_listen_without_a_device_raises():
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        await hub.listen()

    try:
        asyncio.run(run())
    except RuntimeError as exc:
        assert "no device connected" in str(exc)
    else:
        raise AssertionError("expected RuntimeError with no device attached")


def test_a_holding_line_does_not_start_the_follow_up_timer():
    """`more=True` means "still working". Without it the Conversation treats the
    holding line as the finished reply, starts the 12s silence window, and the
    device re-arms its wake word while the brain is still looking something up —
    the real answer then plays to a crab that has already gone to sleep."""
    async def run():
        conv = Conversation(window_s=12.0, reply_timeout_s=300.0)
        hub = Hub(FakeTTS(), send_wav=lambda ws, path: asyncio.sleep(0))
        hub.attach(FakeWS(), conv)
        conv.opened()
        conv.utterance_submitted()          # the user spoke; a reply is owed
        await hub.speak("Let me have a look.", more=True)
        return conv

    conv = asyncio.run(run())
    assert not conv.should_end(), "the turn must stay open while we work"


def test_the_real_reply_does_start_the_follow_up_timer():
    async def run():
        conv = Conversation(window_s=12.0, reply_timeout_s=300.0)
        hub = Hub(FakeTTS(), send_wav=lambda ws, path: asyncio.sleep(0))
        hub.attach(FakeWS(), conv)
        conv.opened()
        conv.utterance_submitted()
        await hub.speak("Here's the answer.")   # more defaults to False
        return conv

    conv = asyncio.run(run())
    # the window is now counting silence rather than waiting on the agent
    assert not conv.should_end()             # not yet — 12s of silence needed
    conv._last_activity -= 13                # fast-forward past the window
    assert conv.should_end()
