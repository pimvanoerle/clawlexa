"""End-to-end handshake test against the real server handler over loopback.

No device and no pytest-asyncio needed — we drive an event loop with
asyncio.run() and connect a client to an ephemeral port.
"""
import asyncio
import functools
import json

import websockets

from clawlexa_bridge import server
from clawlexa_bridge.stt import FakeSTT
from clawlexa_bridge.tts import FakeTTS


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


def test_audio_session_writes_wav(tmp_path, monkeypatch):
    """A begin -> binary frames -> end session lands a WAV under recordings/."""
    monkeypatch.chdir(tmp_path)  # AudioRecorder writes to ./recordings

    async def run():
        async with websockets.serve(server.handle_connection, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(b"\x01\x02\x03\x04" * 64)  # 256 bytes of PCM
                await ws.send(json.dumps({"type": "audio_end"}))
                await asyncio.sleep(0.2)  # let the server flush + close the WAV

    asyncio.run(run())
    import glob
    import wave
    wavs = glob.glob(str(tmp_path / "recordings" / "*.wav"))
    assert len(wavs) == 1
    with wave.open(wavs[0], "rb") as w:
        assert w.getframerate() == 16000
        assert w.getnframes() == 128  # 256 bytes / 2 bytes per sample


def test_transcribes_on_audio_end(tmp_path, monkeypatch):
    """After audio_end, the bridge runs the saved recording through STT."""
    monkeypatch.chdir(tmp_path)
    fake = FakeSTT("hello clawlexa")

    async def run():
        handler = functools.partial(server.handle_connection, stt=fake)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(b"\x01\x02" * 128)
                await ws.send(json.dumps({"type": "audio_end"}))
                await asyncio.sleep(0.3)  # let to_thread STT finish

    asyncio.run(run())
    assert len(fake.calls) == 1  # transcribed exactly one saved WAV


def test_speaks_reply_on_audio_end(tmp_path, monkeypatch):
    """After transcribing, the bridge synthesizes a reply and streams it back."""
    monkeypatch.chdir(tmp_path)
    fake_stt = FakeSTT("hello clawlexa")
    fake_tts = FakeTTS()

    async def run():
        handler = functools.partial(server.handle_connection, stt=fake_stt, tts=fake_tts)
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(b"\x01\x02" * 128)
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
