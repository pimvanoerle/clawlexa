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


def demo_reply(text: str) -> str:
    """The 'agent'. A real agent (iPinch) replaces this with its LLM — the point
    is that the *agent*, not the bridge, decides what to say. A few toy intents
    here so the reply is audibly agent-chosen, not a mechanical echo."""
    t = text.lower().strip(" .!?")
    if t in ("hi", "hello", "hey") or t.startswith(("hello", "hi ")):
        return "Hey there! I'm the demo agent, talking to you over MCP."
    if "weather" in t:
        return "I can't check the weather yet, but the whole MCP loop is working."
    if "your name" in t or "who are you" in t:
        return "I'm clawlexa — your words reached an agent through MCP and back."
    if "thank" in t:
        return "You're welcome!"
    return f"The agent heard you say: {text}"


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
                reply = demo_reply(text)  # a real agent's LLM goes here
                print(f"[demo] reply: {reply!r}", file=sys.stderr)
                await session.call_tool("speak", {"text": reply})


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
