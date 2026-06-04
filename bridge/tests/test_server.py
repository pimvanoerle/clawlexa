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


def test_recording_is_echoed_back_for_playback(tmp_path, monkeypatch):
    """After audio_end, the bridge streams the recording back: play_begin ->
    binary PCM -> play_end, and the echoed PCM matches what was sent."""
    monkeypatch.chdir(tmp_path)
    pcm = bytes(range(256)) * 8  # 2048 bytes

    async def run():
        async with websockets.serve(server.handle_connection, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.send(json.dumps({"type": "audio_begin", "rate": 16000,
                                          "channels": 1, "bits": 16}))
                await ws.send(pcm)
                await ws.send(json.dumps({"type": "audio_end"}))
                begin = json.loads(await asyncio.wait_for(ws.recv(), 3))
                audio = b""
                while True:
                    m = await asyncio.wait_for(ws.recv(), 3)
                    if isinstance(m, (bytes, bytearray)):
                        audio += m
                    else:
                        return begin, audio, json.loads(m)

    begin, audio, end = asyncio.run(run())
    assert begin["type"] == "play_begin"
    assert (begin["rate"], begin["channels"], begin["bits"]) == (16000, 1, 16)
    assert end["type"] == "play_end"
    assert audio == pcm
