"""Integration tests for the MCP surface — a real MCP client talks to the real
server over the SDK's in-memory transport. No device, no subprocess: exercises
the full protocol path (client -> tool dispatch -> Hub) at build time.
"""
import asyncio

from mcp.shared.memory import create_connected_server_and_client_session as connected

from clawlexa_bridge.hub import Hub
from clawlexa_bridge.mcp_server import build_mcp
from clawlexa_bridge.tts import FakeTTS


class FakeWS:
    """A device connection that records the control frames sent to it."""

    def __init__(self):
        self.sent = []

    async def send(self, frame):
        self.sent.append(frame)


def _text(result):
    """First text block of a CallToolResult."""
    return result.content[0].text


def test_client_lists_the_tools():
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        async with connected(build_mcp(hub)) as client:
            tools = await client.list_tools()
            return sorted(t.name for t in tools.tools)

    assert asyncio.run(run()) == ["end_conversation", "listen", "set_state", "show",
                                  "speak", "wait_for_utterance"]


def test_wait_for_utterance_round_trip():
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        async with connected(build_mcp(hub)) as client:
            await hub.submit_utterance("turn on the lights")
            return _text(await client.call_tool("wait_for_utterance", {"timeout_ms": 1000}))

    assert asyncio.run(run()) == "turn on the lights"


def test_speak_reaches_the_device():
    sent = []

    async def fake_send(ws, path):
        sent.append(path)

    async def run():
        hub = Hub(FakeTTS(), send_wav=fake_send)
        hub.attach("WS")
        async with connected(build_mcp(hub)) as client:
            return _text(await client.call_tool("speak", {"text": "hello there"}))

    assert asyncio.run(run()) == "ok"
    assert len(sent) == 1  # the reply WAV reached the (fake) device


def test_wait_for_utterance_times_out_to_empty():
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        async with connected(build_mcp(hub)) as client:
            return _text(await client.call_tool("wait_for_utterance", {"timeout_ms": 50}))

    assert asyncio.run(run()) == ""  # nothing said within the window


def test_listen_is_served_while_wait_for_utterance_is_blocked():
    """The ambient greeting (SPEC §7a) calls tools from a second task while the
    voice loop sits blocked in wait_for_utterance. If the server dispatched
    requests one at a time that would deadlock — and only ever live, on a real
    device. Prove it here instead.
    """
    async def run():
        hub = Hub(FakeTTS(), send_wav=None)
        ws = FakeWS()
        hub.attach(ws)
        async with connected(build_mcp(hub)) as client:
            waiting = asyncio.create_task(
                client.call_tool("wait_for_utterance", {"timeout_ms": 5000}))
            await asyncio.sleep(0)  # let it reach the server and block
            # ... the greeting task opens a listening window meanwhile
            opened = await asyncio.wait_for(client.call_tool("listen", {}), timeout=5)
            await hub.submit_utterance("hey pinchy")
            return _text(opened), _text(await asyncio.wait_for(waiting, timeout=5))

    opened, heard = asyncio.run(run())
    assert opened == "ok"
    assert heard == "hey pinchy"
