"""End-to-end handshake test against the real server handler over loopback.

No device and no pytest-asyncio needed — we drive an event loop with
asyncio.run() and connect a client to an ephemeral port.
"""
import array
import asyncio
import functools
import json

import websockets

from clawlexa_bridge import server
from clawlexa_bridge.conversation import Conversation
from clawlexa_bridge.stt import FakeSTT
from clawlexa_bridge.tts import FakeTTS
from clawlexa_bridge.vad import Endpointer

FRAME_SAMPLES = 320  # 20 ms @ 16 kHz


def voiced(n_frames: int) -> bytes:
    return array.array("h", [6000] * (FRAME_SAMPLES * n_frames)).tobytes()


def silence(n_frames: int) -> bytes:
    return b"\x00\x00" * (FRAME_SAMPLES * n_frames)


def fast_ep(rate):
    """A snappy endpointer for tests: starts in 2 frames, ends after 3 silent."""
    return Endpointer(rate=rate, start_ms=40, end_silence_ms=60, pre_roll_ms=40,
                      threshold=300)


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


def test_voiced_session_writes_utterance_wav(tmp_path, monkeypatch):
    """A begin -> voiced PCM -> end session lands a VAD utterance WAV."""
    monkeypatch.chdir(tmp_path)  # AudioRecorder writes to ./recordings

    async def run():
        handler = functools.partial(server.handle_connection, endpointer_factory=fast_ep)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(voiced(4))
                await ws.send(json.dumps({"type": "audio_end"}))  # flush utterance
                await asyncio.sleep(0.2)  # let the server flush + close the WAV

    asyncio.run(run())
    import glob
    import wave
    wavs = glob.glob(str(tmp_path / "recordings" / "*.wav"))
    assert len(wavs) == 1
    with wave.open(wavs[0], "rb") as w:
        assert w.getframerate() == 16000
        assert w.getnframes() > 0


def test_silence_writes_no_wav(tmp_path, monkeypatch):
    """Pure silence never crosses the VAD threshold -> no utterance, no WAV."""
    monkeypatch.chdir(tmp_path)

    async def run():
        handler = functools.partial(server.handle_connection, endpointer_factory=fast_ep)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(silence(10))
                await ws.send(json.dumps({"type": "audio_end"}))
                await asyncio.sleep(0.2)

    asyncio.run(run())
    import glob
    assert glob.glob(str(tmp_path / "recordings" / "*.wav")) == []


def test_transcribes_utterance_on_flush(tmp_path, monkeypatch):
    """audio_end flushes the in-progress utterance through STT."""
    monkeypatch.chdir(tmp_path)
    fake = FakeSTT("hello clawlexa")

    async def run():
        handler = functools.partial(server.handle_connection, stt=fake,
                                    endpointer_factory=fast_ep)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(voiced(4))
                await ws.send(json.dumps({"type": "audio_end"}))
                await asyncio.sleep(0.3)  # let to_thread STT finish

    asyncio.run(run())
    assert len(fake.calls) == 1  # transcribed exactly one utterance


def test_vad_endpoints_midstream_without_audio_end(tmp_path, monkeypatch):
    """A trailing-silence gap ends an utterance mid-stream (no audio_end needed)."""
    monkeypatch.chdir(tmp_path)
    fake = FakeSTT("midstream")

    async def run():
        handler = functools.partial(server.handle_connection, stt=fake,
                                    endpointer_factory=fast_ep)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(voiced(4) + silence(5))  # speech + endpointing gap
                await asyncio.sleep(0.3)  # no audio_end — VAD must endpoint on its own

    asyncio.run(run())
    assert len(fake.calls) == 1


def test_speaks_reply_per_utterance(tmp_path, monkeypatch):
    """After transcribing an utterance, the bridge synthesizes + streams a reply."""
    monkeypatch.chdir(tmp_path)
    fake_stt = FakeSTT("hello clawlexa")
    fake_tts = FakeTTS()

    async def run():
        handler = functools.partial(server.handle_connection, stt=fake_stt,
                                    tts=fake_tts, endpointer_factory=fast_ep)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(voiced(4))
                await ws.send(json.dumps({"type": "audio_end"}))
                frames = []
                while True:  # collect the playback stream until play_end
                    msg = await asyncio.wait_for(ws.recv(), timeout=3)
                    frames.append(msg)
                    if isinstance(msg, str) and json.loads(msg).get("type") == "play_end":
                        break
                return frames

    frames = asyncio.run(run())
    assert fake_tts.calls == ["You said: hello clawlexa"]  # reply built from transcript
    types = [json.loads(f)["type"] for f in frames if isinstance(f, str)]
    assert types[0] == "play_begin" and types[-1] == "play_end"
    assert any(isinstance(f, (bytes, bytearray)) for f in frames)  # PCM was sent


def test_empty_transcript_is_skipped(tmp_path, monkeypatch):
    """An utterance that transcribes to nothing (silence/echo) gets no reply."""
    monkeypatch.chdir(tmp_path)
    fake_stt = FakeSTT("   ")  # whitespace only -> treated as no speech
    fake_tts = FakeTTS()

    async def run():
        handler = functools.partial(server.handle_connection, stt=fake_stt,
                                    tts=fake_tts, endpointer_factory=fast_ep)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(voiced(4))
                await ws.send(json.dumps({"type": "audio_end"}))
                await asyncio.sleep(0.3)

    asyncio.run(run())
    assert fake_tts.calls == []  # no reply synthesized for empty speech


def test_conversation_idle_sends_end_turn():
    """After a wake with no speech, the bridge tells the device to re-arm."""
    async def run():
        # A snappy window so the test doesn't wait the real 7s.
        conv_factory = lambda: Conversation(window_s=0.1, reply_timeout_s=1.0)
        handler = functools.partial(server.handle_connection,
                                    conversation_factory=conv_factory,
                                    watchdog_tick_s=0.02)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                # No speech: after the window, the bridge should send end_turn.
                msg = await asyncio.wait_for(ws.recv(), timeout=3)
                return json.loads(msg)

    msg = asyncio.run(run())
    assert msg["type"] == "end_turn"


def test_looks_like_noise():
    from clawlexa_bridge.server import looks_like_noise
    assert looks_like_noise("yeah yeah yeah yeah yeah")            # pure repetition
    assert looks_like_noise("Let's go. Let's go. Let's go. Let's go.")  # phrase repeat
    assert not looks_like_noise("what time is the meeting today")  # real sentence
    assert not looks_like_noise("bye")                            # short + real
    assert not looks_like_noise("okay thanks pinchy")             # short + real


def test_noise_transcript_is_skipped(tmp_path, monkeypatch):
    """A hallucination-style transcript (one word repeated) gets no reply."""
    monkeypatch.chdir(tmp_path)
    fake_stt = FakeSTT("yeah yeah yeah yeah yeah yeah")
    fake_tts = FakeTTS()

    async def run():
        handler = functools.partial(server.handle_connection, stt=fake_stt,
                                    tts=fake_tts, endpointer_factory=fast_ep)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(voiced(4))
                await ws.send(json.dumps({"type": "audio_end"}))
                await asyncio.sleep(0.3)

    asyncio.run(run())
    assert fake_tts.calls == []  # noise was filtered before any reply


def test_mcp_mode_routes_utterance_to_hub(tmp_path, monkeypatch):
    """With a hub (MCP mode), the transcript goes to the agent — no echo reply."""
    monkeypatch.chdir(tmp_path)
    from clawlexa_bridge.hub import Hub
    fake_stt = FakeSTT("turn on the lights")
    sent = []

    async def fake_send(ws, path):
        sent.append(path)

    hub = Hub(FakeTTS(), send_wav=fake_send)

    async def run():
        handler = functools.partial(server.handle_connection, stt=fake_stt,
                                    hub=hub, endpointer_factory=fast_ep)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(voiced(4))
                await ws.send(json.dumps({"type": "audio_end"}))
                return await hub.next_utterance(timeout=2)

    text = asyncio.run(run())
    assert text == "turn on the lights"  # handed to the agent
    assert sent == []  # MCP mode: no auto-reply pushed to the device
