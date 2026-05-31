#!/usr/bin/env python3
"""Capture a mic recording the device dumps over serial into a .wav file.

The firmware records a few seconds from the ICS-43434 on boot and prints it as
base64 framed by MIC_WAV_BEGIN/END. This resets the board, reads that block
(tolerating interleaved log lines), and writes a WAV.

Requires a capture-enabled build (the dump is off by default):
    idf.py -DCONFIG_CLAWLEXA_MIC_DUMP_ON_BOOT=y build flash

Usage:
    python3 capture_mic.py [PORT] [OUTFILE]
    # defaults: /dev/cu.usbmodem14301  mic_capture.wav
"""
import base64
import re
import sys
import time
import wave

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem14301"
OUT = sys.argv[2] if len(sys.argv) > 2 else "mic_capture.wav"
B64_RE = re.compile(r"^[A-Za-z0-9+/]+={0,2}$")


def main() -> int:
    s = serial.Serial(PORT, 115200, timeout=1)
    # Pulse reset so the capture happens after we're listening.
    s.setDTR(False); s.setRTS(True); time.sleep(0.15); s.setRTS(False)

    rate, bits, channels = 16000, 16, 1
    chunks, collecting = [], False
    deadline = time.time() + 60
    while time.time() < deadline:
        line = s.readline().decode("utf-8", "replace").strip()
        if line.startswith("MIC_WAV_BEGIN"):
            for tok in line.split():
                if tok.startswith("rate="):
                    rate = int(tok[5:])
                elif tok.startswith("bits="):
                    bits = int(tok[5:])
                elif tok.startswith("ch="):
                    channels = int(tok[3:])
            chunks, collecting = [], True
        elif line.startswith("MIC_WAV_END"):
            break
        elif collecting and B64_RE.match(line):
            chunks.append(line)
    s.close()

    if not chunks:
        print("No mic data captured (no MIC_WAV_BEGIN..END seen).", file=sys.stderr)
        return 1

    data = base64.b64decode("".join(chunks))
    with wave.open(OUT, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(bits // 8)
        w.setframerate(rate)
        w.writeframes(data)
    print(f"wrote {OUT}: {len(data)} bytes, {rate} Hz, {channels}ch/{bits}-bit, "
          f"{len(data) / (rate * channels * bits // 8):.2f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
