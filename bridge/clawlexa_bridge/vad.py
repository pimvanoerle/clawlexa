"""Server-side voice-activity endpointing.

The device streams mic audio continuously; the bridge decides where utterances
begin and end (SPEC §6: energy-based VAD for v1). Endpointer is a pure state
machine — feed it 16-bit mono PCM as it arrives and it returns complete
utterances (as PCM byte blobs) when it sees enough trailing silence. No IO, no
deps, so it's fully host-testable with synthetic audio.

Shape of an utterance: [pre-roll] + speech + [trailing silence up to the
hangover]. Pre-roll prepends a little audio from *before* speech crossed the
threshold so word onsets aren't clipped; the hangover keeps short pauses inside
one utterance from splitting it.
"""
from __future__ import annotations

import array
import math
from collections import deque


def rms(frame: bytes) -> float:
    """Root-mean-square amplitude of a 16-bit mono PCM frame (0 for empty)."""
    samples = array.array("h")
    samples.frombytes(frame)
    if not samples:
        return 0.0
    return math.sqrt(sum(s * s for s in samples) / len(samples))


class Endpointer:
    # Defaults grounded in real captures: idle/silence frames sit around 60-80
    # RMS, so threshold 400 clears the noise floor by ~5x while still catching
    # quieter speech; an 800 ms hangover keeps natural mid-sentence pauses from
    # splitting one utterance into fragments.
    def __init__(self, rate: int = 16000, frame_ms: int = 20,
                 threshold: float = 400.0, start_ms: int = 100,
                 end_silence_ms: int = 800, pre_roll_ms: int = 200,
                 max_utterance_ms: int = 20000) -> None:
        self.frame_bytes = int(rate * frame_ms / 1000) * 2  # 16-bit samples
        self.threshold = threshold
        self.start_frames = max(1, start_ms // frame_ms)
        self.end_frames = max(1, end_silence_ms // frame_ms)
        self.max_frames = max(1, max_utterance_ms // frame_ms)
        self._preroll: deque[bytes] = deque(maxlen=max(0, pre_roll_ms // frame_ms))
        self._buf = bytearray()       # bytes not yet a whole frame
        self._utter: list[bytes] = []  # frames in the in-progress utterance
        self._in_speech = False
        self._voiced_run = 0          # consecutive voiced frames (arming start)
        self._silence_run = 0         # consecutive silent frames (arming end)

    @property
    def in_speech(self) -> bool:
        """True while an utterance is in progress — the bridge uses this to keep
        the conversation window open while the user is mid-sentence."""
        return self._in_speech

    def feed(self, pcm: bytes) -> list[bytes]:
        """Consume PCM; return any utterances that completed within it."""
        self._buf += pcm
        out: list[bytes] = []
        while len(self._buf) >= self.frame_bytes:
            frame = bytes(self._buf[:self.frame_bytes])
            del self._buf[:self.frame_bytes]
            done = self._process(frame)
            if done is not None:
                out.append(done)
        return out

    def flush(self) -> bytes | None:
        """End of stream: emit the current utterance (if speaking), else None."""
        if self._buf and self._in_speech:
            self._utter.append(bytes(self._buf))  # trailing partial frame
        self._buf = bytearray()
        if self._in_speech and self._utter:
            return self._finish()
        self._reset()
        return None

    def _process(self, frame: bytes) -> bytes | None:
        voiced = rms(frame) >= self.threshold
        if not self._in_speech:
            self._preroll.append(frame)
            self._voiced_run = self._voiced_run + 1 if voiced else 0
            if self._voiced_run >= self.start_frames:
                self._in_speech = True
                self._utter = list(self._preroll)  # include pre-roll context
                self._preroll.clear()
                self._silence_run = 0
            return None

        self._utter.append(frame)
        self._silence_run = 0 if voiced else self._silence_run + 1
        if self._silence_run >= self.end_frames or len(self._utter) >= self.max_frames:
            return self._finish()
        return None

    def _finish(self) -> bytes:
        utter = b"".join(self._utter)
        self._reset()
        return utter

    def _reset(self) -> None:
        self._utter = []
        self._in_speech = False
        self._voiced_run = 0
        self._silence_run = 0
        self._preroll.clear()
