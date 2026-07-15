"""WebSocket server the clawlexa device connects to.

Phase 2b: accept the device, handle the hello -> welcome handshake, and log
frames. Binary (audio) frames are logged as a placeholder until Phase 2c.
"""
from __future__ import annotations

import asyncio
import functools
import logging
import re
import wave
from collections import Counter

import websockets

from .audio import AudioRecorder
from .conversation import Conversation
from .discovery import advertise
from .protocol import (end_turn_message, parse_message, play_begin_message,
                       play_end_message, welcome_message)
from .stt import STT, WhisperSTT
from .tts import TTS, PiperTTS
from .vad import Endpointer

log = logging.getLogger("clawlexa.bridge")

# Bytes of PCM per binary frame sent to the device (512 samples @ 16-bit).
PLAY_CHUNK_BYTES = 1024


def reply_text(transcript: str) -> str:
    """The phrase the bridge speaks back for a given transcript (3b loopback)."""
    return f"You said: {transcript}"


def looks_like_noise(text: str) -> bool:
    """True for transcripts that are almost certainly STT hallucination on
    silence/echo rather than real speech — chiefly the "yeah yeah yeah…" style
    where one short word dominates the whole utterance. Conservative: genuine
    speech isn't mostly a single repeated token, and short utterances (which might
    be a real "bye" or "yes") are always let through."""
    words = re.findall(r"[a-z']+", text.lower())
    if len(words) < 4:
        return False  # too short to judge as repetition
    _, count = Counter(words).most_common(1)[0]
    return count / len(words) >= 0.5


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
                            stt: STT | None, tts: TTS | None, hub=None,
                            conv: Conversation | None = None) -> bool:
    """Save one VAD-segmented utterance, transcribe it, and dispatch it.

    With a `hub` (MCP mode), the transcript goes to the agent, which replies via
    the `speak` tool — returns False (no immediate reply). Standalone, it echoes
    "You said: X" and returns True so the caller can apply half-duplex backoff.
    STT/TTS are blocking — kept off the event loop with to_thread. `conv` (if set)
    is told a reply is now owed so the conversation window holds open (SPEC §7).
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
    if looks_like_noise(text):  # STT hallucination on echo/noise — don't queue it
        log.info("utterance looks like noise (%r); skipping", text[:48])
        return False
    log.info('you said: "%s"', text)
    if conv is not None:  # a real transcript: the agent now owes a reply
        conv.utterance_submitted()
    if hub is not None:  # MCP mode: hand the transcript to the agent
        await hub.submit_utterance(text)
        return False
    if tts is not None:  # standalone loopback
        wav = await asyncio.to_thread(tts.synthesize, reply_text(text))
        if conv is not None:
            conv.reply_started()
        await send_wav(ws, wav)
        if conv is not None:
            conv.reply_finished()
        return True
    return False


async def handle_connection(ws: websockets.WebSocketServerProtocol,
                            stt: STT | None = None,
                            tts: TTS | None = None,
                            endpointer_factory=None,
                            hub=None,
                            conversation_factory=None,
                            watchdog_tick_s: float = 0.25) -> None:
    peer = ws.remote_address
    log.info("device connected from %s", peer)
    make_ep = endpointer_factory or (lambda rate: Endpointer(rate=rate))
    make_conv = conversation_factory or (lambda: Conversation())
    conv = make_conv()  # bridge-driven multi-turn window (SPEC §7)
    if hub is not None:
        hub.attach(ws, conv)
    ep: Endpointer | None = None  # set between audio_begin and audio_end
    rate = 16000

    async def watchdog() -> None:
        """When the conversation goes idle, tell the device to stop streaming and
        re-arm its wake word. Runs alongside the recv loop; the only thing that
        ends a conversation now that the firmware no longer times itself out."""
        nonlocal ep
        try:
            while True:
                await asyncio.sleep(watchdog_tick_s)
                if conv.should_end():
                    await ws.send(end_turn_message())
                    conv.closed()
                    ep = None  # ignore any frames until the next audio_begin
                    log.info("conversation over -> end_turn (re-arm wake)")
        except (asyncio.CancelledError, websockets.ConnectionClosed):
            pass

    watch = asyncio.create_task(watchdog())
    try:
        async for message in ws:
            # Binary frames are PCM from the device's continuous mic stream;
            # the VAD endpointer slices them into utterances.
            if isinstance(message, (bytes, bytearray)):
                if ep is None:
                    continue  # stray audio before audio_begin (or after end_turn)
                for utt in ep.feed(message):
                    spoke = await _handle_utterance(ws, utt, rate, stt, tts, hub, conv)
                    if spoke:
                        # Half-duplex: we just played a reply. Reset the
                        # endpointer so any of our own audio the mic caught
                        # doesn't get treated as a new utterance.
                        ep = make_ep(rate)
                        break
                # Mid-sentence speech keeps the follow-up window from re-arming.
                if ep is not None and ep.in_speech:
                    conv.voice_activity()
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
                conv.opened()  # wake fired: a conversation is now live
                log.info("stream open (%dHz) — VAD endpointing", rate)
            elif mtype == "audio_end":
                if ep is not None:
                    utt = ep.flush()
                    if utt:
                        await _handle_utterance(ws, utt, rate, stt, tts, hub, conv)
                    ep = None
                conv.closed()
                log.info("stream closed")
            else:
                log.info("recv %r", msg)
    except websockets.ConnectionClosed:
        pass
    finally:
        watch.cancel()
        if ep is not None:  # device dropped mid-stream — keep the last utterance
            utt = ep.flush()
            if utt:
                await _handle_utterance(ws, utt, rate, stt, tts, hub, conv)
        conv.closed()
        if hub is not None:
            hub.detach(ws)
        log.info("device disconnected from %s", peer)


async def serve(host: str, port: int, stt: STT | None = None,
                tts: TTS | None = None, vad_threshold: float | None = None,
                vad_end_silence_ms: int | None = None, hub=None) -> None:
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
    handler = functools.partial(handle_connection, stt=stt, tts=tts, hub=hub,
                                endpointer_factory=lambda rate: Endpointer(rate=rate, **ep_kwargs))
    log.info("clawlexa-bridge listening on ws://%s:%d", host, port)
    # Advertise over mDNS so the device can find us by service type, not a fixed
    # IP (best-effort; the device falls back to its Kconfig BRIDGE_HOST).
    with advertise(port):
        async with websockets.serve(handler, host, port):
            await asyncio.Future()  # run until cancelled
