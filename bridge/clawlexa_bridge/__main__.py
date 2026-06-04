"""Entry point: `python -m clawlexa_bridge [--host H] [--port P]`."""
from __future__ import annotations

import argparse
import asyncio
import logging

from .server import serve


def main() -> None:
    parser = argparse.ArgumentParser(prog="clawlexa-bridge")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default: all)")
    parser.add_argument("--port", type=int, default=8765, help="listen port (default: 8765)")
    parser.add_argument("--vad-threshold", type=float, default=None,
                        help="VAD energy (RMS) threshold; higher = less sensitive "
                             "(default: Endpointer's 400)")
    parser.add_argument("--vad-end-silence-ms", type=int, default=None,
                        help="trailing silence that ends an utterance, ms "
                             "(default: Endpointer's 800)")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        asyncio.run(serve(args.host, args.port, vad_threshold=args.vad_threshold,
                          vad_end_silence_ms=args.vad_end_silence_ms))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
