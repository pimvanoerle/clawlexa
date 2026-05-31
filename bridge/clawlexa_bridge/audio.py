"""Write a streamed PCM session from the device to a .wav file.

The device frames a recording as: an `audio_begin` control message (with the
PCM format), then raw binary PCM frames, then `audio_end`. AudioRecorder turns
that into a WAV on disk. Kept separate from the socket code so it's unit-testable.
"""
from __future__ import annotations

import os
import time
import wave


class AudioRecorder:
    def __init__(self, outdir: str = "recordings") -> None:
        self.outdir = outdir
        self._wav: wave.Wave_write | None = None
        self.path: str | None = None
        self._frames = 0
        self._seq = 0

    def begin(self, rate: int, channels: int, bits: int) -> str:
        """Open a fresh timestamped WAV for a new recording session."""
        self.end()  # finalize any session left open
        os.makedirs(self.outdir, exist_ok=True)
        # ms timestamp + a per-recorder counter, so back-to-back sessions differ
        self._seq += 1
        self.path = os.path.join(self.outdir, f"clawlexa_{int(time.time()*1000)}_{self._seq}.wav")
        self._wav = wave.open(self.path, "wb")
        self._wav.setnchannels(channels)
        self._wav.setsampwidth(bits // 8)
        self._wav.setframerate(rate)
        self._frames = 0
        return self.path

    def write(self, pcm: bytes) -> None:
        if self._wav is not None:
            self._wav.writeframes(pcm)
            self._frames += len(pcm)

    def active(self) -> bool:
        return self._wav is not None

    def end(self) -> tuple[str | None, int]:
        """Close the WAV; returns (path, bytes_written) or (None, 0) if idle."""
        if self._wav is None:
            return None, 0
        self._wav.close()
        self._wav = None
        return self.path, self._frames
