"""MCP server for AppleWinHarness (spec: docs/specs/m5.md).

The adapter launches the harness (--serve), connects as operator (to create the session, step frames and close it)
and as agent (to type, look, listen), and serves nine tools over MCP stdio.

stdout carries MCP frames only: the harness child's stdout is a pipe that harness_client drains, and all logging goes
to stderr. The adapter writes the line "harness pid <N>" to stderr once the harness is running.
"""

import argparse
import asyncio
import base64
import json
import logging
import math
import os
import sys
import threading
import time

from mcp.server.mcpserver import Image, MCPServer
from mcp.server.mcpserver.exceptions import ToolError

from . import __version__
from .harness_client import ConnectionClosed, Harness, RpcError

FRAMES_PER_SECOND = 60
INPUT_FRAME_CAP = 600      # frames type_text / press_key wait for the keyboard to be read
DEFAULT_HOLD_MS = 50       # the harness's press_key default
SETTLE_FRAMES = 3          # frames after the last key so the program can echo it
CALL_TIMEOUT = 600.0
WAIT_RANGE = (1, 60000)
LISTEN_RANGE = (0, 60000)


_stderr_lock = threading.Lock()   # the adapter's lines and the forwarded harness lines never interleave mid-line
_logger = logging.getLogger("applewin_mcp")


def _level_name(levelno):
    if levelno >= logging.ERROR:
        return "error"
    if levelno >= logging.WARNING:
        return "warning"
    return "info" if levelno >= logging.INFO else "debug"


class JsonFormatter(logging.Formatter):
    """One JSON object per line, the harness's shape: {ts, level, msg, component}."""

    def format(self, record):
        ms = int(record.msecs)
        ts = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(record.created)) + ".{:03d}Z".format(ms)
        msg = record.getMessage()
        if record.exc_info:
            msg += "\n" + self.formatException(record.exc_info)
        return json.dumps({"ts": ts, "level": _level_name(record.levelno), "msg": msg, "component": "adapter"})


class TextFormatter(logging.Formatter):
    """The harness's text shape: "level: message", one line."""

    def format(self, record):
        msg = record.getMessage()
        if record.exc_info:
            msg += "\n" + self.formatException(record.exc_info)
        return "{}: {}".format(_level_name(record.levelno), msg.replace("\r", "\\r").replace("\n", "\\n"))


class _StderrHandler(logging.Handler):
    def emit(self, record):
        try:
            data = (self.format(record) + "\n").encode("utf-8", "replace")
            write_stderr(data)
        except Exception:
            self.handleError(record)


def write_stderr(data):
    """Writes whole bytes to stderr under the lock."""
    with _stderr_lock:
        try:
            sys.stderr.buffer.write(data)
            sys.stderr.buffer.flush()
        except (AttributeError, ValueError, OSError):
            pass


def configure_logging(log_format):
    handler = _StderrHandler()
    handler.setFormatter(JsonFormatter() if log_format == "json" else TextFormatter())
    root = logging.getLogger()
    root.handlers[:] = [handler]   # the MCP SDK's own records go through the same formatter
    root.setLevel(logging.INFO)


def log(message):
    _logger.info(message)


def parse_args(argv=None):
    p = argparse.ArgumentParser(prog="applewin_mcp", description="MCP server for an emulated Apple //e")
    p.add_argument("--exe", required=True, help="path to AppleWinHarness.exe")
    p.add_argument("--headless", action="store_true", help="run the harness without a window")
    p.add_argument("--disk", help="disk image for slot 6 drive 1 (the harness works on a copy)")
    p.add_argument("--realtime", action="store_true", help="run in realtime mode (default: stepped)")
    p.add_argument("--log-format", choices=("text", "json"), default="text",
                   help="stderr log format, also passed to the harness (default: text)")
    p.add_argument("--version", action="version", version="applewin_mcp " + __version__)
    p.add_argument("--text-screen", action="store_true", help="enable the read_screen tool")
    return p.parse_args(argv)


def _check_range(name, value, low, high):
    if not low <= value <= high:
        raise ToolError("{} must be between {} and {} (got {})".format(name, low, high, value))


class Machine:
    """The running harness and its two connections. Methods block; the tools call them through a worker thread,
    and self.lock keeps one tool call at a time on the emulator."""

    def __init__(self, args):
        self.stepped = not args.realtime
        self.lock = threading.Lock()
        self.events = []        # sound events not yet reported by listen (buffered from launch)
        self.applied = set()    # input ids whose input_applied has arrived
        os.environ["HARNESS_EXE"] = os.path.abspath(args.exe)
        self.harness = Harness.launch(headless=args.headless, extra_args=["--log-format", args.log_format],
                                      stderr_line=self._forward_harness_line)
        log("harness pid {}".format(self.harness.pid))
        try:
            self.operator = self.harness.connect("operator")
            params = {"run_mode": "stepped" if self.stepped else "realtime",
                      "allow_text_screen": bool(args.text_screen)}
            if args.disk:
                params["disks"] = [{"slot": 6, "drive": 1, "path": os.path.abspath(args.disk)}]
            self.operator.call("create_session", timeout=CALL_TIMEOUT, **params)
            self.agent = self.harness.connect("agent")
            self.agent.call("subscribe_audio", timeout=CALL_TIMEOUT)
        except BaseException:
            self.shutdown()
            raise

    @staticmethod
    def _forward_harness_line(line):
        """The harness's stderr, line by line and unchanged (so in json mode each line is still one JSON object)."""
        if not line.endswith(b"\n"):
            line += b"\n"
        write_stderr(line)

    # ---- helpers (call with self.lock held) ----

    def rpc(self, conn, method, **params):
        try:
            return conn.call(method, timeout=CALL_TIMEOUT, **params)
        except RpcError as e:
            raise ToolError(e.message) from None
        except (ConnectionClosed, TimeoutError, OSError) as e:
            raise ToolError("the emulator is not responding ({!r})".format(e)) from None

    def pump(self, timeout=0.0):
        """Moves queued notifications into self.events / self.applied."""
        wait = timeout
        while True:
            try:
                note = self.agent.next_notification(timeout=wait)
            except TimeoutError:
                return
            wait = 0.0
            method, params = note.get("method"), note.get("params") or {}
            if method == "input_applied":
                self.applied.add(params.get("input_id"))
            elif method in ("sound_started", "sound_stopped"):
                self.events.append((method, params))

    def advance(self, frames):
        """Lets `frames` video frames of emulated time pass (stepped: step_frames; realtime: sleeps)."""
        if self.stepped:
            self.rpc(self.operator, "step_frames", frames=frames)
        else:
            time.sleep(frames / FRAMES_PER_SECOND)

    def advance_ms(self, ms):
        if self.stepped:
            self.advance(math.ceil(ms * FRAMES_PER_SECOND / 1000))
        else:
            time.sleep(ms / 1000.0)

    def await_input(self, input_id, expected_ms):
        """Lets time pass, a frame at a time, until input_applied arrives for input_id. The cap counts frames beyond
        expected_ms, the time the input itself takes (keys x delay, or the hold)."""
        frames = 0
        allowed = math.ceil(expected_ms * FRAMES_PER_SECOND / 1000) + INPUT_FRAME_CAP
        while True:
            self.pump(0.02 if self.stepped else 0.0)
            if input_id in self.applied:
                self.applied.discard(input_id)
                self.advance(SETTLE_FRAMES)   # let the program echo the last key
                return
            if frames >= allowed:
                raise ToolError("the keyboard wasn't read within {} frames: the program may not be reading keys "
                                "(it may be busy). Call wait to let time pass, then look.".format(INPUT_FRAME_CAP))
            self.advance(1)
            frames += 1

    # ---- tools ----

    def type_text(self, text, inter_key_delay_ms):
        with self.lock:
            reply = self.rpc(self.agent, "type_text", text=text, inter_key_delay_ms=inter_key_delay_ms)
            self.await_input(reply["input_id"], len(text) * inter_key_delay_ms)
        return "typed {} characters".format(len(text))

    def press_key(self, key, hold_ms):
        params = {"key": key}
        if hold_ms > 0:
            params["hold_ms"] = hold_ms
        with self.lock:
            reply = self.rpc(self.agent, "press_key", **params)
            self.await_input(reply["input_id"], hold_ms if hold_ms > 0 else DEFAULT_HOLD_MS)
        return "pressed {}".format(key)

    def set_joystick(self, x, y, button0, button1, duration_ms):
        params = {}
        if x is not None:
            params["x"] = x
        if y is not None:
            params["y"] = y
        buttons = {}
        if button0 is not None:
            buttons["0"] = button0
        if button1 is not None:
            buttons["1"] = button1
        if buttons:
            params["buttons"] = buttons
        if duration_ms > 0:
            params["duration_ms"] = duration_ms
        with self.lock:
            self.rpc(self.agent, "set_joystick", **params)
        return "joystick set; call wait to let time pass"

    def wait(self, ms):
        with self.lock:
            self.advance_ms(ms)
        return "waited {} ms".format(ms)

    def look(self):
        with self.lock:
            frame = self.rpc(self.agent, "get_frame", format="PNG", borders=False)
        return base64.b64decode(frame["data"])

    def read_screen(self):
        with self.lock:
            screen = self.rpc(self.agent, "get_text_screen")
        return "\n".join([screen["mode"]] + screen["lines"])

    def listen(self, ms):
        with self.lock:
            if ms > 0:
                self.advance_ms(ms)
            self.pump(0.0)
            events, self.events = self.events, []
        if not events:
            return "no sound events since the last listen"
        lines = []
        for method, p in events:
            if method == "sound_started":
                lines.append("sound_started emulated_ms={}".format(p.get("emulated_ms")))
            else:
                lines.append("sound_stopped duration_ms={} peak_db={} dominant_hz={}".format(
                    p.get("duration_ms"), p.get("peak_db"), p.get("dominant_hz")))
        return "\n".join(lines)

    def insert_disk(self, drive, path, write_protected):
        path = os.path.abspath(path)   # relative paths are taken against the adapter's working directory
        if path.startswith(("\\\\", "//")):   # UNC shares and device paths (\?\, \.\): the agent is the untrusted side
            raise ToolError("UNC and device paths (starting with \\ or //) are not allowed; use a local path")
        with self.lock:
            self.rpc(self.operator, "insert_disk", slot=6, drive=drive, path=path,
                     write_protected=write_protected)
        return "inserted {} in drive {}".format(path, drive)

    def eject_disk(self, drive):
        with self.lock:
            self.rpc(self.operator, "eject_disk", slot=6, drive=drive)
        return "ejected the disk from drive {}".format(drive)

    def shutdown(self):
        """close_session, then wait for the process to exit (Harness.close waits 5 s, then kills it)."""
        try:
            self.harness.close()
        except Exception as e:   # never let cleanup hide the reason we are stopping
            log("shutdown error: {!r}".format(e))
            self.harness.kill()


def build_server(args, machine):
    mcp = MCPServer("applewin")

    @mcp.tool()
    async def type_text(text: str, inter_key_delay_ms: int = 0) -> str:
        """Type text on the Apple II keyboard (\\n is RETURN; printable ASCII only, at most 4096 characters).
        Blocks until the keyboard has read the keys, so you can look at the screen straight away. If the program
        is not reading the keyboard this fails with an error. To let time pass (for a program to run), call wait."""
        return await asyncio.to_thread(machine.type_text, text, inter_key_delay_ms)

    @mcp.tool()
    async def press_key(key: str | int, hold_ms: int = 0) -> str:
        """Press one key: a character, an ASCII code (an integer 0-127, e.g. 13 for
        RETURN), or a name such as RETURN, ESC, TAB, DELETE, LEFT, RIGHT, UP, DOWN, CTRL-A to CTRL-Z,
        or RESET. Blocks until the keyboard has read it. If the program is not reading the keyboard this fails with an error. To let time pass, call wait."""
        return await asyncio.to_thread(machine.press_key, key, hold_ms)

    @mcp.tool()
    async def set_joystick(x: int | None = None, y: int | None = None, button0: bool | None = None,
                           button1: bool | None = None, duration_ms: int = 0) -> str:
        """Move joystick 0 (x, y 0-255; 127 is centre) and/or press its buttons (button0 is Open-Apple, button1
        Solid-Apple). Fields left out keep their value. With duration_ms the joystick then returns to its previous
        state. Returns at once; call wait to let time pass."""
        return await asyncio.to_thread(machine.set_joystick, x, y, button0, button1, duration_ms)

    @mcp.tool()
    async def wait(ms: int) -> str:
        """Let emulated time pass (1-60000 ms). Typing and other input only take effect as time passes, and
        programs only run while time passes."""
        _check_range("ms", ms, *WAIT_RANGE)
        return await asyncio.to_thread(machine.wait, ms)

    @mcp.tool()
    async def look() -> Image:
        """Look at the screen: a 560x384 PNG."""
        return Image(data=await asyncio.to_thread(machine.look), format="png")

    if args.text_screen:
        @mcp.tool()
        async def read_screen() -> str:
            """Read the text on the screen: the video mode (40col, 80col, mixed40, mixed80 or graphics), then 24 lines."""
            return await asyncio.to_thread(machine.read_screen)

    @mcp.tool()
    async def listen(ms: int = 0) -> str:
        """Listen for sound, optionally letting ms (0-60000) of time pass first. Returns the sound_started and
        sound_stopped events (with duration_ms and dominant_hz) since the last listen."""
        _check_range("ms", ms, *LISTEN_RANGE)
        return await asyncio.to_thread(machine.listen, ms)

    @mcp.tool()
    async def insert_disk(drive: int, path: str, write_protected: bool = False) -> str:
        """Put a floppy disk image in the disk drive (drive 1 or 2; drives are numbered 1 and 2), replacing any disk
        already there. The machine keeps running and is NOT rebooted: the program on the screen does not notice
        until it reads the disk. After a swap, DOS needs a command such as CATALOG (or CATALOG,D2 for drive 2) to
        see the new disk. path is a disk image file (a relative path is taken from the server's working
        directory; paths starting with two backslashes or two slashes are rejected). The original file is never written to; the machine works on a copy. Set write_protected to
        stop the machine writing to the disk."""
        return await asyncio.to_thread(machine.insert_disk, drive, path, write_protected)

    @mcp.tool()
    async def eject_disk(drive: int) -> str:
        """Take the floppy out of the disk drive (drive 1 or 2; drives are numbered 1 and 2). The machine keeps
        running and is not rebooted. Ejecting from an empty drive is an error."""
        return await asyncio.to_thread(machine.eject_disk, drive)

    return mcp


def main(argv=None):
    args = parse_args(argv)
    configure_logging(args.log_format)
    machine = Machine(args)
    try:
        build_server(args, machine).run("stdio")
    finally:
        machine.shutdown()
    return 0
