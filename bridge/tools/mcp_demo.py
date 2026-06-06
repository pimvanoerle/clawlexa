#!/usr/bin/env python3
"""Demo MCP client for the clawlexa bridge — the agent loop, in miniature.

Spawns the bridge as an MCP server (`python -m clawlexa_bridge --mcp`) over
stdio, then loops: wait for the user to speak (after the on-device wake word
fires) and speak a reply back. It's both a live end-to-end demo (with a device
connected) and the reference for wiring a real agent like iPinch — swap the
canned "You said: X" reply for your agent's response.

Run from the bridge/ directory (needs the device powered + on WiFi, and nothing
else already bound to the bridge's WS port):

    .venv/bin/python tools/mcp_demo.py

Everything prints to stderr — stdout belongs to the MCP stdio protocol.
"""
from __future__ import annotations

import asyncio
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def _text(result) -> str:
    return result.content[0].text if result.content else ""


async def main() -> None:
    # Spawn the bridge as our MCP server. This same StdioServerParameters block
    # is what a real agent (iPinch) would put in its MCP server config.
    params = StdioServerParameters(command=sys.executable,
                                   args=["-m", "clawlexa_bridge", "--mcp"])
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = await session.list_tools()
            print(f"[demo] connected; tools: {[t.name for t in tools.tools]}", file=sys.stderr)
            print("[demo] say your wake word then talk; Ctrl-C to quit", file=sys.stderr)
            while True:
                text = _text(await session.call_tool("wait_for_utterance", {}))
                if not text:
                    continue
                print(f"[demo] heard: {text!r}", file=sys.stderr)
                # A real agent thinks here. We just echo it back.
                await session.call_tool("speak", {"text": f"You said: {text}"})


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
