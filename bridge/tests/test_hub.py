"""Unit tests for the Hub — the device-link <-> MCP-agent bridge (Phase 5).

No MCP transport, no real device: the agent loop is exercised directly.
"""
import asyncio
import json
import os

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


def test_next_utterance_times_out():
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        try:
            await hub.next_utterance(timeout=0.05)
            return "no-timeout"
        except asyncio.TimeoutError:
            return "timeout"

    assert asyncio.run(run()) == "timeout"


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
