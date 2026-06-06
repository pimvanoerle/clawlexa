#!/usr/bin/env python3
"""Cycle the device through every display state so you can see all the crab
faces (and the show() text), driven over MCP — a visual smoke test for Phase 6.

Run from bridge/ with the device powered + on WiFi:

    .venv/bin/python tools/crab_parade.py
"""
from __future__ import annotations

import asyncio
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

STATES = ["idle", "listening", "thinking", "speaking", "error"]
HOLD_S = 2.5  # how long to show each state


async def main() -> None:
    params = StdioServerParameters(command=sys.executable,
                                   args=["-m", "clawlexa_bridge", "--mcp"])
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            print("[parade] connected — watch the screen", file=sys.stderr)
            await asyncio.sleep(2)  # let the device connect over WiFi
            for state in STATES:
                print(f"[parade] {state}", file=sys.stderr)
                await session.call_tool("set_state", {"state": state})
                await asyncio.sleep(HOLD_S)
            await session.call_tool("show", {"text": "hello :)"})
            await asyncio.sleep(HOLD_S)
            await session.call_tool("set_state", {"state": "idle"})
            print("[parade] done", file=sys.stderr)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
