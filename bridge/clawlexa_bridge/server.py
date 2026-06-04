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
from .tts import TTS, PiperTTS
from .vad import Endpointer

log = logging.getLogger("clawlexa.bridge")

# Bytes of PCM per binary frame sent to the device (512 samples @ 16-bit).
PLAY_CHUNK_BYTES = 1024


def reply_text(transcript: str) -> str:
    """The phrase the bridge speaks back for a given transcript (3b loopback)."""
    return f"You said: {transcript}"


async def send_wav(ws, path: str) -> None:
    """Stream a WAV to the device to play: play_begin -> binary PCM -> play_end."""
    with wave.open(path, "rb") as w:
        rate, channels, bits = w.getframerate(), w.getnchannels(), w.getsampwidth() * 8
        nframes = w.getnframes()
        data = w.readframes(nframes)
    ms = round(nframes * 1000 / rate)  # so the device can mute for the full reply
    await ws.send(play_begin_message(rate, channels, bits, ms=ms))
    for i in range(0, len(data), PLAY_CHUNK_BYTES):
        await ws.send(data[i:i + PLAY_CHUNK_BYTES])
    await ws.send(play_end_message())
    log.info("sent %d bytes of PCM (%d ms) to device for playback", len(data), ms)


async def _handle_utterance(ws, pcm: bytes, rate: int,
                            stt: STT | None, tts: TTS | None) -> bool:
    """Save one VAD-segmented utterance, transcribe it, and speak the reply.

    Returns True if a spoken reply was sent (so the caller can apply half-duplex
    backoff). STT/TTS are blocking — kept off the event loop with to_thread.
    """
    rec = AudioRecorder()
    rec.begin(rate, 1, 16)
    rec.write(pcm)
    path, nbytes = rec.end()
    log.info("utterance -> %s (%d bytes)", path, nbytes)
    if not (path and stt is not None):
        return False
    text = await asyncio.to_thread(stt.transcribe, path)
    if not text.strip():  # silence/noise/our own echo tail — nothing to say back
        log.info("utterance had no speech; skipping")
        return False
    log.info('you said: "%s"', text)
    if tts is not None:
        wav = await asyncio.to_thread(tts.synthesize, reply_text(text))
        await send_wav(ws, wav)
        return True
    return False


async def handle_connection(ws: websockets.WebSocketServerProtocol,
                            stt: STT | None = None,
                            tts: TTS | None = None,
                            endpointer_factory=None) -> None:
    peer = ws.remote_address
    log.info("device connected from %s", peer)
    make_ep = endpointer_factory or (lambda rate: Endpointer(rate=rate))
    ep: Endpointer | None = None  # set between audio_begin and audio_end
    rate = 16000
    try:
        async for message in ws:
            # Binary frames are PCM from the device's continuous mic stream;
            # the VAD endpointer slices them into utterances.
            if isinstance(message, (bytes, bytearray)):
                if ep is None:
                    continue  # stray audio before audio_begin
                for utt in ep.feed(message):
                    spoke = await _handle_utterance(ws, utt, rate, stt, tts)
                    if spoke:
                        # Half-duplex: we just played a reply. Reset the
                        # endpointer so any of our own audio the mic caught
                        # doesn't get treated as a new utterance.
                        ep = make_ep(rate)
                        break
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
                rate = msg.get("rate", 16000)
                ep = make_ep(rate)
                log.info("stream open (%dHz) — VAD endpointing", rate)
            elif mtype == "audio_end":
                if ep is not None:
                    utt = ep.flush()
                    if utt:
                        await _handle_utterance(ws, utt, rate, stt, tts)
                    ep = None
                log.info("stream closed")
            else:
                log.info("recv %r", msg)
    except websockets.ConnectionClosed:
        pass
    finally:
        if ep is not None:  # device dropped mid-stream — keep the last utterance
            utt = ep.flush()
            if utt:
                await _handle_utterance(ws, utt, rate, stt, tts)
        log.info("device disconnected from %s", peer)


async def serve(host: str, port: int, stt: STT | None = None,
                tts: TTS | None = None, vad_threshold: float | None = None,
                vad_end_silence_ms: int | None = None) -> None:
    if stt is None:
        log.info("loading STT model (faster-whisper)...")
        stt = WhisperSTT()
    if tts is None:
        log.info("loading TTS voice (piper)...")
        tts = PiperTTS()
    # Build endpointers with any tuned VAD knobs (None -> Endpointer defaults).
    ep_kwargs = {}
    if vad_threshold is not None:
        ep_kwargs["threshold"] = vad_threshold
    if vad_end_silence_ms is not None:
        ep_kwargs["end_silence_ms"] = vad_end_silence_ms
    handler = functools.partial(handle_connection, stt=stt, tts=tts,
                                endpointer_factory=lambda rate: Endpointer(rate=rate, **ep_kwargs))
    log.info("clawlexa-bridge listening on ws://%s:%d", host, port)
    async with websockets.serve(handler, host, port):
        await asyncio.Future()  # run until cancelled
