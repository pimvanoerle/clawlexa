"""Speech-to-text for the bridge.

Local `faster-whisper` by default; behind a small interface so a cloud engine
(or a fake for tests) can drop in without touching the server. faster-whisper is
imported lazily inside WhisperSTT so importing this module — and the tests that
use FakeSTT — don't need the (heavy) dependency or a model download.
"""
from __future__ import annotations

from abc import ABC, abstractmethod


class STT(ABC):
    @abstractmethod
    def transcribe(self, wav_path: str) -> str:
        """Return the transcript of a 16 kHz mono WAV file."""


class WhisperSTT(STT):
    def __init__(self, model_size: str = "base.en", device: str = "cpu",
                 compute_type: str = "int8") -> None:
        from faster_whisper import WhisperModel  # lazy: heavy import

        self._model = WhisperModel(model_size, device=device, compute_type=compute_type)

    def transcribe(self, wav_path: str) -> str:
        segments, _info = self._model.transcribe(wav_path, language="en")
        return " ".join(seg.text.strip() for seg in segments).strip()


class FakeSTT(STT):
    """Deterministic STT for tests — returns a preset transcript, loads nothing."""

    def __init__(self, text: str = "(fake transcript)") -> None:
        self.text = text
        self.calls: list[str] = []

    def transcribe(self, wav_path: str) -> str:
        self.calls.append(wav_path)
        return self.text
