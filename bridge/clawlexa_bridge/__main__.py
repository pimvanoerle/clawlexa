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
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        asyncio.run(serve(args.host, args.port))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
