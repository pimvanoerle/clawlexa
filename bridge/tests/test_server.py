"""End-to-end handshake test against the real server handler over loopback.

No device and no pytest-asyncio needed — we drive an event loop with
asyncio.run() and connect a client to an ephemeral port.
"""
import asyncio
import json

import websockets

from clawlexa_bridge import server


def test_hello_gets_welcome():
    async def run():
        async with websockets.serve(server.handle_connection, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "hello", "device": "clawlexa"}))
                reply = await asyncio.wait_for(ws.recv(), timeout=3)
                return json.loads(reply)

    msg = asyncio.run(run())
    assert msg["type"] == "welcome"
    assert msg["bridge"] == "clawlexa-bridge"


def test_bad_frame_does_not_crash_connection():
    """A garbage control frame is dropped; the connection still serves a hello."""
    async def run():
        async with websockets.serve(server.handle_connection, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send("garbage not json")
                await ws.send(json.dumps({"type": "hello"}))
                reply = await asyncio.wait_for(ws.recv(), timeout=3)
                return json.loads(reply)

    msg = asyncio.run(run())
    assert msg["type"] == "welcome"
