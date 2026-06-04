"""Text-to-speech for the bridge.

Local Piper by default, behind a small interface so a cloud engine (or a fake
for tests) can drop in without touching the server. Piper is imported lazily
inside PiperTTS so importing this module — and the tests that use FakeTTS —
needs neither the dependency nor a downloaded voice.

The default voice is a **16 kHz "low"** model on purpose: Piper's "low" voices
synthesize at 16 kHz, which drops straight onto our 16 kHz mono wire with no
resampling. The device plays incoming PCM at a fixed 16 kHz clock (it ignores
play_begin's rate on the binary path), so a 22 kHz "medium" voice would come
out fast/high-pitched.
"""
from __future__ import annotations

import os
import tempfile
import wave
from abc import ABC, abstractmethod
from pathlib import Path

DEFAULT_VOICE = "en_US-lessac-low"  # 16 kHz native — matches the device's I2S clock


class TTS(ABC):
    @abstractmethod
    def synthesize(self, text: str) -> str:
        """Render `text` to a 16 kHz mono 16-bit WAV file and return its path."""


class PiperTTS(TTS):
    def __init__(self, voice: str = DEFAULT_VOICE, voices_dir: str = "voices") -> None:
        from piper import PiperVoice  # lazy: heavy import

        vdir = Path(voices_dir)
        model = vdir / f"{voice}.onnx"
        if not model.exists():
            from piper.download_voices import download_voice  # lazy

            vdir.mkdir(parents=True, exist_ok=True)
            download_voice(voice, vdir)
        self._voice = PiperVoice.load(str(model))

    def synthesize(self, text: str) -> str:
        fd, path = tempfile.mkstemp(suffix=".wav", prefix="tts_")
        os.close(fd)
        with wave.open(path, "wb") as wf:
            self._voice.synthesize_wav(text, wf)
        return path


class FakeTTS(TTS):
    """Deterministic TTS for tests — writes a short silent 16 kHz WAV, loads nothing."""

    def __init__(self, frames: int = 160) -> None:
        self.frames = frames
        self.calls: list[str] = []

    def synthesize(self, text: str) -> str:
        self.calls.append(text)
        fd, path = tempfile.mkstemp(suffix=".wav", prefix="faketts_")
        os.close(fd)
        with wave.open(path, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(16000)
            wf.writeframes(b"\x00\x00" * self.frames)
        return path
