"""WebSocket server the clawlexa device connects to.

Phase 2b: accept the device, handle the hello -> welcome handshake, and log
frames. Binary (audio) frames are logged as a placeholder until Phase 2c.
"""
from __future__ import annotations

import asyncio
import logging

import websockets

from .protocol import parse_message, welcome_message

log = logging.getLogger("clawlexa.bridge")


async def handle_connection(ws: websockets.WebSocketServerProtocol) -> None:
    peer = ws.remote_address
    log.info("device connected from %s", peer)
    try:
        async for message in ws:
            if isinstance(message, (bytes, bytearray)):
                log.info("audio frame: %d bytes (ignored until Phase 2c)", len(message))
                continue
            try:
                msg = parse_message(message)
            except ValueError as exc:
                log.warning("dropping bad control frame: %s", exc)
                continue
            log.info("recv %r", msg)
            if msg.get("type") == "hello":
                await ws.send(welcome_message())
                log.info("sent welcome to %s", msg.get("device", "?"))
    except websockets.ConnectionClosed:
        pass
    finally:
        log.info("device disconnected from %s", peer)


async def serve(host: str, port: int) -> None:
    log.info("clawlexa-bridge listening on ws://%s:%d", host, port)
    async with websockets.serve(handle_connection, host, port):
        await asyncio.Future()  # run until cancelled
