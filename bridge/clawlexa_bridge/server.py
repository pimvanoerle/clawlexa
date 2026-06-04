"""WebSocket server the clawlexa device connects to.

Phase 2b: accept the device, handle the hello -> welcome handshake, and log
frames. Binary (audio) frames are logged as a placeholder until Phase 2c.
"""
from __future__ import annotations

import asyncio
import functools
import logging
import wave

import websockets

from .audio import AudioRecorder
from .protocol import parse_message, play_begin_message, play_end_message, welcome_message
from .stt import STT, WhisperSTT

log = logging.getLogger("clawlexa.bridge")

# Bytes of PCM per binary frame sent to the device (512 samples @ 16-bit).
PLAY_CHUNK_BYTES = 1024


async def send_wav(ws, path: str) -> None:
    """Stream a WAV to the device to play: play_begin -> binary PCM -> play_end."""
    with wave.open(path, "rb") as w:
        rate, channels, bits = w.getframerate(), w.getnchannels(), w.getsampwidth() * 8
        data = w.readframes(w.getnframes())
    await ws.send(play_begin_message(rate, channels, bits))
    for i in range(0, len(data), PLAY_CHUNK_BYTES):
        await ws.send(data[i:i + PLAY_CHUNK_BYTES])
    await ws.send(play_end_message())
    log.info("sent %d bytes of PCM to device for playback", len(data))


async def handle_connection(ws: websockets.WebSocketServerProtocol,
                            stt: STT | None = None) -> None:
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
                if path and stt is not None:
                    # Transcription can be slow; don't block the event loop.
                    text = await asyncio.to_thread(stt.transcribe, path)
                    log.info('you said: "%s"', text)
            else:
                log.info("recv %r", msg)
    except websockets.ConnectionClosed:
        pass
    finally:
        if rec.active():  # device dropped mid-stream — keep what we got
            path, nbytes = rec.end()
            log.info("saved %s (%d bytes, on disconnect)", path, nbytes)
        log.info("device disconnected from %s", peer)


async def serve(host: str, port: int, stt: STT | None = None) -> None:
    if stt is None:
        log.info("loading STT model (faster-whisper)...")
        stt = WhisperSTT()
    handler = functools.partial(handle_connection, stt=stt)
    log.info("clawlexa-bridge listening on ws://%s:%d", host, port)
    async with websockets.serve(handler, host, port):
        await asyncio.Future()  # run until cancelled
