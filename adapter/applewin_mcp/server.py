"""MCP server for AppleWinHarness (spec: docs/specs/m5.md).

TASK 1 STUB: the command line is parsed for real and the seven tools are registered with their final names and
parameters, but every tool raises NotImplementedError (an MCP tool error). Task 3 implements them.

Contract the tests rely on: stdout carries MCP frames only; the adapter writes the line "harness pid <N>" to stderr
once the harness is running.
"""

import argparse
import sys

from mcp.server.mcpserver import MCPServer


def parse_args(argv=None):
    p = argparse.ArgumentParser(prog="applewin_mcp", description="MCP server for an emulated Apple //e")
    p.add_argument("--exe", required=True, help="path to AppleWinHarness.exe")
    p.add_argument("--headless", action="store_true", help="run the harness without a window")
    p.add_argument("--disk", help="disk image for slot 6 drive 1 (the harness works on a copy)")
    p.add_argument("--realtime", action="store_true", help="run in realtime mode (default: stepped)")
    p.add_argument("--text-screen", action="store_true", help="enable the read_screen tool")
    return p.parse_args(argv)


def build_server(args):
    mcp = MCPServer("applewin")

    @mcp.tool()
    def type_text(text: str, inter_key_delay_ms: int = 0) -> str:
        """Type text on the Apple II keyboard (\n is RETURN). Returns once the keys have been typed."""
        raise NotImplementedError("type_text")

    @mcp.tool()
    def press_key(key: str, hold_ms: int = 0) -> str:
        """Press one key: a character, an ASCII code, or a name such as RETURN, ESC, LEFT."""
        raise NotImplementedError("press_key")

    @mcp.tool()
    def set_joystick(x: int | None = None, y: int | None = None, button0: bool | None = None,
                     button1: bool | None = None, duration_ms: int = 0) -> str:
        """Move joystick 0 (x, y 0-255; 127 is centre) and/or press its buttons."""
        raise NotImplementedError("set_joystick")

    @mcp.tool()
    def wait(ms: int) -> str:
        """Let emulated time pass (1-60000 ms). Typing and other input only take effect as time passes."""
        raise NotImplementedError("wait")

    @mcp.tool()
    def look():
        """Look at the screen: a 560x384 PNG."""
        raise NotImplementedError("look")

    if args.text_screen:
        @mcp.tool()
        def read_screen() -> str:
            """Read the text on the screen: the video mode, then 24 lines."""
            raise NotImplementedError("read_screen")

    @mcp.tool()
    def listen(ms: int = 0) -> str:
        """Listen for sound (optionally waiting ms first): sound start/stop events since the last listen."""
        raise NotImplementedError("listen")

    return mcp


def main(argv=None):
    args = parse_args(argv)
    build_server(args).run("stdio")
    return 0
