"""WebSocket server the clawlexa device connects to.

Phase 2b: accept the device, handle the hello -> welcome handshake, and log
frames. Binary (audio) frames are logged as a placeholder until Phase 2c.
"""
from __future__ import annotations

import asyncio
import logging

import websockets

from .audio import AudioRecorder
from .protocol import parse_message, welcome_message

log = logging.getLogger("clawlexa.bridge")


async def handle_connection(ws: websockets.WebSocketServerProtocol) -> None:
    peer = ws.remote_address
    log.info("device connected from %s", peer)
    rec = AudioRecorder()
    try:
        async for message in ws:
            # Binary frames are PCM audio for the active recording session.
            if isinstance(message, (bytes, bytearray)):
                rec.write(message)
                continue
            try:
                msg = parse_message(message)
            except ValueError as exc:
                log.warning("dropping bad control frame: %s", exc)
                continue
            mtype = msg.get("type")
            if mtype == "hello":
                await ws.send(welcome_message())
                log.info("sent welcome to %s", msg.get("device", "?"))
            elif mtype == "audio_begin":
                path = rec.begin(msg.get("rate", 16000), msg.get("channels", 1),
                                 msg.get("bits", 16))
                log.info("recording -> %s (%dHz)", path, msg.get("rate", 16000))
            elif mtype == "audio_end":
                path, nbytes = rec.end()
                log.info("saved %s (%d bytes)", path, nbytes)
            else:
                log.info("recv %r", msg)
    except websockets.ConnectionClosed:
        pass
    finally:
        if rec.active():  # device dropped mid-stream — keep what we got
            path, nbytes = rec.end()
            log.info("saved %s (%d bytes, on disconnect)", path, nbytes)
        log.info("device disconnected from %s", peer)


async def serve(host: str, port: int) -> None:
    log.info("clawlexa-bridge listening on ws://%s:%d", host, port)
    async with websockets.serve(handle_connection, host, port):
        await asyncio.Future()  # run until cancelled
