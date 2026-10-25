# SPDX-License-Identifier: MIT
"""MCP server for AppleWinHarness (spec: docs/specs/m5.md).

The adapter launches the harness (--serve), connects as operator (to create the session, step frames and close it)
and as agent (to type, look, listen), and serves thirteen tools over MCP stdio, plus four checkpoint tools in a stepped session.

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
import queue
import sys
import threading
import time

from mcp.server.mcpserver import Image, MCPServer
from mcp.server.mcpserver.exceptions import ToolError

from . import __version__
from .harness_client import ConnectionClosed, Harness, RpcError
from .screen import (MAX_SHEET_H, MAX_SHEET_W, ScreenError, check_region, check_size, check_zoom,
                     contact_sheet, crop_zoom, encode_png, frame_pixels, region_differs, sheet_size)

FRAMES_PER_SECOND = 60
BASE_CLOCK_HZ = (157500000.0 / 11.0) * 65.0 / 912.0   # CLK_6502_NTSC: emulated cycles to emulated ms (D10)
INPUT_FRAME_CAP = 600      # frames per key the input tools wait beyond the input's own time (the harness's timeout)
DEFAULT_HOLD_MS = 50       # the harness's press_key default
SETTLE_FRAMES = 3          # frames after the last key so the program can echo it
CALL_TIMEOUT = 600.0
WAIT_RANGE = (1, 60000)
LISTEN_RANGE = (0, 60000)
CHANGE_RANGE = (1, 60000)
STRIP_FRAMES_RANGE = (1, 16)
STRIP_EVERY_MS_RANGE = (1, 1000)
STRIP_PER_ROW = 4
STRIP_GAP = 2


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


class StderrWriter:
    """Every stderr line (forwarded harness lines and the adapter's own records) goes through one bounded queue to a
    single daemon thread, so lines stay whole, and a slow or full stderr never blocks a producer: the line is dropped
    and counted instead. The next line the thread reaches is preceded by one warning saying how many were dropped."""

    def __init__(self, maxsize=10000, target=None):
        self.queue = queue.Queue(maxsize)
        self.target = target or self._write_stderr
        self.dropped = 0
        self.dropped_line = lambda n: "warning: {} log lines dropped\n".format(n).encode("ascii")
        self._lock = threading.Lock()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    @staticmethod
    def _write_stderr(data):
        try:
            sys.stderr.buffer.write(data)
            sys.stderr.buffer.flush()
        except (AttributeError, ValueError, OSError):
            pass

    def write(self, data):
        try:
            self.queue.put_nowait(data)
        except queue.Full:
            with self._lock:
                self.dropped += 1

    def _run(self):
        while True:
            data = self.queue.get()
            if data is None:
                self.queue.task_done()
                return
            with self._lock:
                dropped, self.dropped = self.dropped, 0
            try:
                if dropped:
                    self.target(self.dropped_line(dropped))
                self.target(data)
            except Exception:
                pass
            self.queue.task_done()

    def close(self, timeout=2.0):
        """Drains what is queued, waiting at most timeout seconds (stderr may be blocked)."""
        try:
            self.queue.put(None, timeout=timeout)
        except queue.Full:
            return
        self.thread.join(timeout)


_writer = StderrWriter()


def write_stderr(data):
    """Queues one complete line (bytes) for stderr; never blocks."""
    _writer.write(data)


def configure_logging(log_format):
    handler = _StderrHandler()
    handler.setFormatter(JsonFormatter() if log_format == "json" else TextFormatter())
    root = logging.getLogger()
    root.handlers[:] = [handler]   # the MCP SDK's own records go through the same formatter
    root.setLevel(logging.INFO)
    def dropped_line(n):   # a warning record in the active format
        record = logging.LogRecord("applewin_mcp", logging.WARNING, "", 0, "{} log lines dropped".format(n), None, None)
        return (handler.format(record) + "\n").encode("utf-8")
    _writer.dropped_line = dropped_line


def cycles_to_ms(cycles):
    return cycles * 1000.0 / BASE_CLOCK_HZ


def _entry_name(entry):
    """How key_report names a press_keys entry: "joystick", its key, or the bare key itself."""
    if not isinstance(entry, dict):
        return entry
    return "joystick" if "joystick" in entry else entry.get("key")


def key_report(keys, timings):
    """press_keys' result, one line per key, in ms from the first press: when it was pressed and read, "never read",
    or "not pressed" (a timed-out sequence lists only the keys it pressed)."""
    start = timings[0]["applied_cycle"] if timings else 0
    lines = []
    for i, k in enumerate(keys, 1):
        name = _entry_name(k)
        if i > len(timings):
            lines.append("{}. {}  not pressed".format(i, name))
            continue
        t = timings[i - 1]
        if "read_cycle" not in t:   # a joystick entry: nothing to read
            lines.append("{}. {}  applied {:.1f} ms".format(i, name, cycles_to_ms(t["applied_cycle"] - start)))
            continue
        # The program acts on a key when it reads it: the latency (read - applied) is how late its clock starts.
        read = "never read" if t["read_cycle"] is None else "read {:.1f} ms (latency {:.1f} ms)".format(
            cycles_to_ms(t["read_cycle"] - start), cycles_to_ms(t["read_cycle"] - t["applied_cycle"]))
        lines.append("{}. {}  applied {:.1f} ms  {}".format(i, name, cycles_to_ms(t["applied_cycle"] - start), read))
    return "\n".join(lines)


def then_report(applied):
    """until_ready's last line (D10, "Ready input"): how the program went on after the last key, in ms from the first
    press; None without until_ready."""
    then = applied.get("then")
    if then is None:
        return None
    timings = applied.get("keys") or []
    at = cycles_to_ms(applied["last_cycle"] - (timings[0]["applied_cycle"] if timings else applied["first_cycle"]))
    if then == "ready":
        return "then: the program was ready for the next key at {:.1f} ms".format(at)
    if then == "input_changed":
        return ("then: the program began waiting for a different kind of input (probably a prompt or a question) at "
                "{:.1f} ms: look at the screen and answer it".format(at))
    return ("then: the program was not ready for another key within {} frames of the last key: it may still be busy, "
            "or waiting for another kind of input (an answer to a prompt ends this way)".format(INPUT_FRAME_CAP))


def log(message):
    _logger.info(message)


def _speed(text):
    if text in ("authentic", "max"):
        return text
    try:
        value = int(text)
    except ValueError:
        value = -1
    if not 0 <= value <= 40:
        raise argparse.ArgumentTypeError("must be authentic, max or a number from 0 to 40 (got {!r})".format(text))
    return value


def parse_args(argv=None):
    p = argparse.ArgumentParser(prog="applewin_mcp", description="MCP server for an emulated Apple //e")
    p.add_argument("--exe", required=True, help="path to AppleWinHarness.exe")
    p.add_argument("--headless", action="store_true", help="run the harness without a window")
    p.add_argument("--disk", help="disk image for slot 6 drive 1 (the harness works on a copy)")
    p.add_argument("--realtime", action="store_true", help="run in realtime mode (default: stepped)")
    p.add_argument("--speed", type=_speed, metavar="0..40|authentic|max",
                   help="machine speed (the harness's machine_speed): authentic is 10, 40 or max is unthrottled")
    p.add_argument("--no-disk-boost", action="store_true",
                   help="keep the pace constant while a disk drive runs (turns off enhance_disk_speed)")
    p.add_argument("--log-format", choices=("text", "json"), default="text",
                   help="stderr log format, also passed to the harness (default: text)")
    p.add_argument("--version", action="version", version="applewin_mcp " + __version__)
    p.add_argument("--text-screen", action="store_true", help="enable the read_screen tool")
    args = p.parse_args(argv)
    if (args.speed is not None or args.no_disk_boost) and not args.realtime:
        p.error("--speed and --no-disk-boost only apply with --realtime (stepped runs always use authentic speed)")
    return args


def session_params(args):
    """The create_session parameters for the command line."""
    params = {"run_mode": "realtime" if args.realtime else "stepped", "allow_text_screen": bool(args.text_screen)}
    if args.disk:
        params["disks"] = [{"slot": 6, "drive": 1, "path": os.path.abspath(args.disk)}]
    speed = {}
    if args.speed is not None:
        speed["machine_speed"] = args.speed
    if args.no_disk_boost:
        speed["enhance_disk_speed"] = False
    if speed:
        params["speed"] = speed
    return params


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
        self.applied = {}       # input id -> discarded, for each input whose input_applied has arrived
        self.frame_ready = []   # frame_ready params, collected only while wait_until_change is subscribed
        self.collect_frames = False
        self.input_screen = None   # (frame RGB, emulated_cycles) just after the last input was sent
        os.environ["HARNESS_EXE"] = os.path.abspath(args.exe)
        self.harness = Harness.launch(headless=args.headless, extra_args=["--log-format", args.log_format],
                                      stderr_line=self._forward_harness_line)
        log("harness pid {}".format(self.harness.pid))
        try:
            self.operator = self.harness.connect("operator")
            self.operator.call("create_session", timeout=CALL_TIMEOUT, **session_params(args))
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
                self.applied[params.get("input_id")] = params
            elif method in ("sound_started", "sound_stopped"):
                self.events.append((method, params))
            elif method == "frame_ready" and self.collect_frames:
                self.frame_ready.append(params)

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

    def note_input_sent(self):
        """Keeps the screen as it is just after an input was sent, for wait_until_change's since="last_input". In a
        stepped session the program cannot have reacted yet (input takes effect at the next step); in realtime the
        fetch takes a few ms, so a program fast enough to draw within them may already show its reaction. Best
        effort: the input was sent, so a failed fetch only drops the reference. Call with self.lock held, right
        after the input RPC."""
        try:
            self.input_screen = self.frame_rgb()
        except ToolError as e:
            self.input_screen = None
            log("could not keep the screen after an input: {}".format(e))

    def await_input(self, input_id, expected_ms, keys=1, describe=None, settle=True):
        """Lets time pass, a frame at a time, until input_applied arrives for input_id, and returns its params. The
        cap counts frames beyond expected_ms, the time the input itself takes (keys x delay, or the hold): 600 per key,
        since the harness may wait that long on each key before it drops the rest (D10). It is only a safety net.
        describe(params), when given, adds lines to the error for a dropped input. settle false skips the frames
        that let the program echo the last key (a strip taken from that moment instead)."""
        frames = 0
        allowed = math.ceil(expected_ms * FRAMES_PER_SECOND / 1000) + INPUT_FRAME_CAP * max(1, keys)
        while True:
            self.pump(0.02 if self.stepped else 0.0)
            if input_id in self.applied:
                params = self.applied.pop(input_id)
                if params.get("discarded"):   # the harness dropped keys the program did not read (D10)
                    stopped = params.get("stopped")   # wait_for_ready (D10, "Ready input")
                    if stopped == "input_changed":
                        message = ("stopped: the program began waiting for a different kind of input (probably a "
                                   "prompt or a question) before it was ready for the next key, so the rest of the "
                                   "keys were not pressed. Look at the screen, answer it, then send the rest again.")
                    elif stopped == "not_ready":
                        message = ("the program was not ready for the next key within {} frames, so the rest of the "
                                   "keys were not pressed. It may be busy, or waiting for another kind of input. Call "
                                   "wait to let time pass, then look.".format(INPUT_FRAME_CAP))
                    else:
                        message = ("the keyboard wasn't read within {} frames: the program may not be reading keys "
                                   "(it may be busy), and the rest of the keys were dropped. Call wait to let time "
                                   "pass, then look.".format(INPUT_FRAME_CAP))
                    raise ToolError(message + ("\n" + describe(params) if describe else ""))
                if settle:
                    self.advance(SETTLE_FRAMES)   # let the program echo the last key
                return params
            if frames >= allowed:
                raise ToolError("the keyboard wasn't read within {} frames: the program may not be reading keys "
                                "(it may be busy). Call wait to let time pass, then look.".format(INPUT_FRAME_CAP))
            self.advance(1)
            frames += 1

    # ---- tools ----

    def type_text(self, text, inter_key_delay_ms):
        with self.lock:
            reply = self.rpc(self.agent, "type_text", text=text, inter_key_delay_ms=inter_key_delay_ms)
            self.note_input_sent()
            self.await_input(reply["input_id"], len(text) * inter_key_delay_ms, keys=len(text))
        return "typed {} characters".format(len(text))

    def press_key(self, key, hold_ms):
        params = {"key": key}
        if hold_ms > 0:
            params["hold_ms"] = hold_ms
        with self.lock:
            reply = self.rpc(self.agent, "press_key", **params)
            self.note_input_sent()
            self.await_input(reply["input_id"], hold_ms if hold_ms > 0 else DEFAULT_HOLD_MS)
        return "pressed {}".format(key)

    def press_keys(self, keys, look=False, frames=1, every_ms=None, region=None, zoom=None, delay_from="applied",
                   wait_for_ready=False, until_ready=False):
        """The key report; with look, [report, PNG] (region/zoom as for look); with frames > 1, [report and tile
        times, contact sheet] of frames screenshots every_ms apart, the first as soon as the last key has been read
        and released. Strip arguments are checked before any key is sent. delay_from "read" counts each delay from the
        program's read of the key before it instead of from that key's apply (D10, "Read-anchored delays").
        wait_for_ready sends each key only when the program is waiting for a command again (D10, "Ready input");
        until_ready also waits for that after the last key, before the look or strip, and reports it."""
        _check_range("frames", frames, *STRIP_FRAMES_RANGE)
        if every_ms is not None:
            _check_range("every_ms", every_ms, *STRIP_EVERY_MS_RANGE)
        strip = frames != 1
        if strip and look:
            raise ToolError("use look or frames, not both: a strip already shows the screen")
        if (region is not None or zoom is not None) and not (look or strip):
            raise ToolError("region and zoom need look or frames")
        if strip:
            box, zoom = self._strip_args(region, zoom, frames, every_ms)
        elif look and (region is not None or zoom is not None):
            try:   # as _look_locked checks them, but before any key is pressed
                box, scale = check_region(region), 2 if zoom is None else zoom
                check_zoom(scale)
                check_size(box[2] * scale, box[3] * scale)
            except ScreenError as e:
                raise ToolError(str(e)) from None
        rpc_params = {"keys": keys}
        if delay_from != "applied":   # the harness's default; anything else is passed on and validated there
            rpc_params["delay_from"] = delay_from
        if wait_for_ready:
            rpc_params["wait_for_ready"] = True
        if until_ready:   # the harness checks it comes with wait_for_ready
            rpc_params["until_ready"] = True
        with self.lock:
            reply = self.rpc(self.agent, "press_keys", **rpc_params)   # the harness validates every entry
            self.note_input_sent()
            expected_ms = 0   # an upper bound: every delay and every hold, end to end
            for k in keys:
                entry = k if isinstance(k, dict) else {}
                if "joystick" in entry:
                    expected_ms += entry.get("delay_ms", 0) + entry.get("duration_ms", 0)
                else:
                    expected_ms += entry.get("delay_ms", 0) + entry.get("hold_ms", DEFAULT_HOLD_MS)
            applied = self.await_input(reply["input_id"], expected_ms, keys=len(keys) + (1 if until_ready else 0),
                                       describe=lambda params: key_report(keys, params.get("keys") or []),
                                       settle=not strip)
            timings = applied.get("keys") or []
            report = key_report(keys, timings)
            then = then_report(applied)
            if then:
                report += "\n" + then
            if strip:
                png, cycles = self._strip_locked(box, zoom, frames, every_ms)
            elif look:
                png = self._look_locked(region, zoom)
            else:
                return report
        if strip:
            last = timings[-1]["applied_cycle"] if timings else cycles[0]   # the harness reports one per entry
            times = ", ".join("{:.1f}".format(cycles_to_ms(c - last)) for c in cycles)
            report += "\ntile times from the last key's press: {} ms".format(times)
        return [report, png]

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
            self.note_input_sent()
        return "joystick set; call wait to let time pass"

    def wait(self, ms):
        with self.lock:
            self.advance_ms(ms)
        return "waited {} ms".format(ms)

    def frame_rgb(self):
        """(560x384 RGB frame, emulated_cycles) from one get_frame. Call with self.lock held."""
        frame = self.rpc(self.agent, "get_frame", format="BGRA32", borders=False)
        try:
            rgb = frame_pixels(base64.b64decode(frame["data"]), frame["width"], frame["height"])
        except ScreenError as e:
            raise ToolError(str(e)) from None
        return rgb, frame["emulated_cycles"]

    def wait_until_change(self, timeout_ms, region=None, look=True, since="now"):
        """Lets time pass until the pixels in region differ from the reference, or timeout_ms of emulated time have
        passed in this call. The reference is the screen at the start (since "now") or just after the last input was
        sent (since "last_input"; a change already made counts at once). Returns [text, PNG of the whole screen at
        the end] (just the text when look is false); the text's time counts from the reference."""
        _check_range("timeout_ms", timeout_ms, *CHANGE_RANGE)
        if since not in ("now", "last_input"):
            raise ToolError('since must be "now" or "last_input"')
        try:
            box = check_region(region)
        except ScreenError as e:
            raise ToolError(str(e)) from None
        with self.lock:
            cur, start = self.frame_rgb()
            if since == "last_input":
                if self.input_screen is None:
                    raise ToolError('since="last_input": no input has been sent yet')
                ref, ref_cycles = self.input_screen
                changed = region_differs(ref, cur, box)
            else:
                ref, ref_cycles, changed = cur, start, False
            cycles, waited = start, 0.0   # waited: emulated time in this call, which timeout_ms bounds
            if changed:
                pass   # since="last_input" and the screen already differs: no waiting
            elif self.stepped:
                while not changed and waited < timeout_ms:
                    self.advance(1)
                    cur, cycles = self.frame_rgb()
                    waited = cycles_to_ms(cycles - start)
                    changed = region_differs(ref, cur, box)
            else:
                deadline = time.monotonic() + 2 * timeout_ms / 1000.0
                self.frame_ready = []
                self.collect_frames = True
                try:
                    self.rpc(self.agent, "subscribe_frames", every_n_frames=1)
                    while not changed and waited < timeout_ms:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            break
                        self.pump(min(remaining, 0.1))
                        if not self.frame_ready:
                            continue
                        self.frame_ready = []
                        cur, cycles = self.frame_rgb()
                        waited = cycles_to_ms(cycles - start)
                        changed = region_differs(ref, cur, box)
                finally:
                    self.collect_frames = False
                    self.frame_ready = []
                    failing = sys.exc_info()[0] is not None   # an exception is already propagating
                    try:
                        self.rpc(self.agent, "subscribe_frames", every_n_frames=0)
                    except ToolError as e:
                        if not failing:
                            raise    # the wait itself succeeded: the failed unsubscribe is the error
                        log("unsubscribe after a failed wait_until_change also failed: {}".format(e))
        text = "{} {} ms".format("changed after" if changed else "no change in",
                                 int(round(cycles_to_ms(cycles - ref_cycles))))
        if since == "last_input":
            text += " (since the last input)"
        if not look:
            return text
        return [text, encode_png(560, 384, cur)]

    def _look_locked(self, region=None, zoom=None):
        """PNG bytes of the screen. Call with self.lock held."""
        if region is None and zoom is None:
            frame = self.rpc(self.agent, "get_frame", format="PNG", borders=False)
            return base64.b64decode(frame["data"])
        try:
            box = check_region(region)
            zoom = 2 if zoom is None else zoom
            check_zoom(zoom)
            check_size(box[2] * zoom, box[3] * zoom)   # before the frame is fetched or cropped
            rgb, _ = self.frame_rgb()
            width, height, pixels = crop_zoom(rgb, box, zoom)
            return encode_png(width, height, pixels)
        except ScreenError as e:
            raise ToolError(str(e)) from None

    def look(self, region=None, zoom=None, frames=1, every_ms=None):
        """The screen as PNG bytes; with frames > 1, [tile times text, PNG of a contact sheet of the tiles taken
        every_ms apart]. Everything is validated before the first tile, so a rejected call lets no time pass."""
        _check_range("frames", frames, *STRIP_FRAMES_RANGE)
        if every_ms is not None:
            _check_range("every_ms", every_ms, *STRIP_EVERY_MS_RANGE)
        if frames == 1:
            with self.lock:
                return self._look_locked(region, zoom)
        box, zoom = self._strip_args(region, zoom, frames, every_ms)
        with self.lock:
            png, cycles = self._strip_locked(box, zoom, frames, every_ms)
        times = ", ".join("{:.1f}".format(cycles_to_ms(c - cycles[0])) for c in cycles)
        return ["tile times: {} ms".format(times), png]

    @staticmethod
    def _strip_args(region, zoom, frames, every_ms):
        """(region box, zoom) for a strip of frames > 1 tiles, after checking every_ms is given and the whole sheet
        fits, so a rejected strip lets no time pass. frames and every_ms ranges are checked by the caller."""
        if every_ms is None:
            raise ToolError("every_ms is required when frames is more than 1")
        try:
            box = check_region(region)
            zoom = 2 if zoom is None else zoom
            check_zoom(zoom)
            check_size(*sheet_size(box[2] * zoom, box[3] * zoom, frames, STRIP_PER_ROW, STRIP_GAP),
                       MAX_SHEET_W, MAX_SHEET_H)
        except ScreenError as e:
            raise ToolError(str(e)) from None
        return box, zoom

    def _strip_locked(self, box, zoom, frames, every_ms):
        """(contact sheet PNG, each tile's emulated_cycles): frames tiles every_ms apart, the first now. Call with
        self.lock held."""
        tiles, cycles = [], []
        for i in range(frames):
            if i:
                self.advance_ms(every_ms)
            rgb, emulated = self.frame_rgb()
            tiles.append(crop_zoom(rgb, box, zoom))
            cycles.append(emulated)
        width, height, pixels = contact_sheet(tiles, STRIP_PER_ROW, STRIP_GAP)
        return encode_png(width, height, pixels), cycles

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

    @staticmethod
    def _local_path(path):
        path = os.path.abspath(path)   # relative paths are taken against the adapter's working directory
        if path.startswith(("\\\\", "//")):   # UNC shares and device paths (\?\, \.\): the agent is the untrusted side
            raise ToolError("UNC and device paths (starting with \\ or //) are not allowed; use a local path")
        return path

    def insert_disk(self, drive, path, write_protected):
        path = self._local_path(path)
        with self.lock:
            self.rpc(self.operator, "insert_disk", slot=6, drive=drive, path=path,
                     write_protected=write_protected)
        return "inserted {} in drive {}".format(path, drive)

    def export_disk(self, drive, path):
        path = self._local_path(path)
        with self.lock:
            self.rpc(self.operator, "export_disk", slot=6, drive=drive, dest_path=path)
        return "exported drive {} to {}".format(drive, path)

    def set_speed(self, speed, disk_boost):
        if self.stepped:
            raise ToolError("set_speed needs a realtime session (start the server with --realtime); "
                            "stepped runs always use authentic speed")
        params = {}
        if speed is not None:
            try:
                params["machine_speed"] = _speed(speed)
            except argparse.ArgumentTypeError as e:
                raise ToolError("speed " + str(e)) from None
        if disk_boost is not None:
            params["enhance_disk_speed"] = disk_boost
        with self.lock:
            result = self.rpc(self.operator, "set_speed" if params else "get_speed", **params)
        return " ".join("{}={}".format(k, result.get(k)) for k in (
            "machine_speed", "clock_multiplier", "effective_clock_hz", "enhance_disk_speed", "full_speed_override",
            "full_speed_active"))

    def eject_disk(self, drive):
        with self.lock:
            self.rpc(self.operator, "eject_disk", slot=6, drive=drive)
        return "ejected the disk from drive {}".format(drive)

    # ---- checkpoints: stepped sessions only (user ruling 2026-10-20; the tools are not registered in realtime) ----

    def _require_stepped(self):
        if not self.stepped:
            raise ToolError("checkpoints are only available in a stepped session")

    @staticmethod
    def _checkpoint_label(checkpoint_id, name):
        return checkpoint_id if name is None else "{} ({})".format(checkpoint_id, name)

    def checkpoint(self, name=None):
        self._require_stepped()
        name = name or None   # an empty name is no name
        params = {} if name is None else {"name": name}
        with self.lock:
            saved = self.rpc(self.operator, "checkpoint", **params)
        return "saved {} at {:.1f} ms".format(self._checkpoint_label(saved["checkpoint_id"], name),
                                              cycles_to_ms(saved["emulated_cycles"]))

    def restore_checkpoint(self, checkpoint_id):
        """Restores the machine and its disks. Everything the adapter kept from the thrown-away timeline goes too:
        the harness reports the input it discarded (drained here), and the sound events not yet given to listen and
        the last-input screen belong to that timeline."""
        self._require_stepped()
        with self.lock:
            try:   # the name is only for the message: looked up first, so the restore's result is what counts
                rows = self.rpc(self.operator, "list_checkpoints")
                name = next((c.get("name") for c in rows if c["checkpoint_id"] == checkpoint_id), None)
            except ToolError:
                name = None
            restored = self.rpc(self.operator, "restore_checkpoint", checkpoint_id=checkpoint_id)
            # The discarded-input notices and the old timeline's last sound events come on the agent connection,
            # which may not have been read yet. A reply there follows that connection's earlier notifications, so
            # once this no-op unsubscribe returns they have all arrived and can be drained.
            self.rpc(self.agent, "subscribe_frames", every_n_frames=0)
            self.pump(0.0)
            self.applied = {}
            self.events = []
            self.input_screen = None
        return "restored {} at {:.1f} ms".format(self._checkpoint_label(checkpoint_id, name),
                                                 cycles_to_ms(restored["emulated_cycles"]))

    def list_checkpoints(self):
        self._require_stepped()
        with self.lock:
            rows = self.rpc(self.operator, "list_checkpoints")
        if not rows:
            return "no checkpoints"
        return "\n".join("{}  {}  {:.1f} ms".format(c["checkpoint_id"], c.get("name") or "-",
                                                    cycles_to_ms(c["emulated_cycles"])) for c in rows)

    def delete_checkpoint(self, checkpoint_id):
        self._require_stepped()
        with self.lock:
            self.rpc(self.operator, "delete_checkpoint", checkpoint_id=checkpoint_id)
        return "deleted {}".format(checkpoint_id)

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
        or RESET. Like a real keyboard it does not wait for the program: a key the program has not read yet is
        replaced. Returns once the key is released, plus a few frames for the program to react. To let time pass,
        call wait."""
        return await asyncio.to_thread(machine.press_key, key, hold_ms)

    @mcp.tool()
    async def press_keys(keys: list[str | int | dict], look: bool = False, frames: int = 1,
                         every_ms: int | None = None, region: list[int] | None = None, zoom: int | None = None,
                         delay_from: str = "applied", wait_for_ready: bool = False, until_ready: bool = False):
        """Press a timed sequence of keys in one call, timed in the machine's own time: use it for a game manoeuvre
        whose keys must land at exact moments (your thinking time between calls would otherwise sit between them).
        Each entry is a key as for press_key (not RESET), or {"key": ..., "delay_ms": ..., "hold_ms": ...}:
        delay_ms (default 0, up to 10000) is the gap after the previous key was pressed (for the first key, after
        this call); hold_ms (default 50, up to 10000) is how long the key is held, and may outlast later keys. Each
        key after the first also waits until the program has read the keyboard since the previous one, so fast keys
        are never lost (LEFT, LEFT stays two presses); the first key goes in at once, replacing any key left unread.
        Add "wait_for_read": false to a key to skip that wait: it then goes in on schedule and replaces a key the
        program has not read yet, as press_key does (a replaced key is reported "never read"). An entry may instead
        be a joystick move {"joystick": {"x": 0-255, "y": 0-255, "buttons": {"0": true, "1": false}}, "delay_ms": ...,
        "duration_ms": 0-60000}: every field is optional, fields left out keep their value, and with duration_ms the
        joystick then returns to its previous state (overlapping durations restore in due order, each to the state
        before its own entry, so a later restore can re-apply an earlier move; avoid overlapping duration_ms or end
        with an explicit centring entry). By default a joystick entry never waits for a keyboard read; it is timed
        from the previous entry like delay_ms above, so it can land between or alongside keys (with delay_from "read"
        it counts from the read of the key before it, like a key). At most 256 entries.
        Timing relative to the program: it acts on a key when it READS it, which for a program that polls the keyboard
        slowly (a title screen, a game loop) is up to a whole loop period after the key went in, depending on where in
        its loop the key arrived; the report shows this as the key's latency. delay_ms counts from the previous entry's
        apply by default, so the gap the program sees between two keys can vary by that much from run to run (and
        between stepped and realtime sessions). Give delay_from "read" (for the whole call, or {"key": ..., "delay_from":
        "read"|"applied"} for one entry) to count delay_ms from the program's read of the key before instead: the keys
        then keep the same timing relative to the program in every run. Use it for timings tuned against a game's own
        clock, and after a key that starts or restarts that clock (a start key, an unpause). The default stays
        "applied". Entries are applied on their exact cycle, and a key that waits for a read goes in at least 1 ms after
        it (a read-anchored delay under about 2 ms can land up to 1 ms late, at the end of the step holding the read). Returns, per entry, when it was pressed (applied) and, for a key, when the program read it and the latency
        (read minus applied), in ms from the first press. Returns once the last key is read and released. If the
        program does not read a key within 600 frames the rest are dropped and this fails, listing the keys pressed
        (the unread one marked "never read") and those not pressed. With look true it also returns the screen after the
        last key (a 560x384 PNG: each Apple pixel (280x192) is drawn 2x2), so you see the result of the manoeuvre
        without a second call. To watch the manoeuvre play out, give frames (2-16) and every_ms (1-1000) instead of
        look: a strip of screenshots as for look, the first taken as soon as the last key has been read and released,
        with tile times in ms from the last key's press. region and zoom (as for look) apply to look or to the
        strip; region is in Apple pixels, so halve coordinates read off a 560x384 screenshot. Strip arguments are
        checked before any key is pressed.
        wait_for_ready: true sends each key only when the program is waiting for its next command again, instead of
        after a delay you guess: use it for a run of moves in a turn-based game, where a move takes longer the more
        the game has to do (a key sent while it is still working is lost) and a move may bring up a question. A key
        counts as taken only when the program clears the keyboard strobe, and "ready" is the program reading the
        keyboard again from every place it read it from while the first key waited to be taken (its input loop),
        so the call may start while the game is still busy. If the program instead starts
        waiting for a different kind of input before it is ready (a prompt, a question), the rest of the keys are
        NOT pressed and the call fails, listing the keys pressed and those not, so no key lands in the answer;
        answer it, then send the rest. Each key goes in as soon as the program is ready, so delay_ms, joystick
        entries, wait_for_read false and delay_from are not allowed with it (hold_ms is). Not for a
        program that never clears the strobe (Conan): use delay_from "read" there. Returns once the program has
        taken the last key. It cannot answer a prompt and then move in one call: send the answer separately.
        until_ready: true (with wait_for_ready only) makes the call return only once the program is ready for another
        key after the last one, so look or frames shows the finished move and you need not guess a wait. The report
        then ends with a "then:" line: ready (with the time), a different kind of input (the last key brought up a
        prompt; the call still succeeds, since every key was pressed, so look and answer it), or not ready within 600
        frames (still busy). Ready is the program's input loop, not the screen: a game that draws after it starts
        waiting again can show a frame early. Do not use it to answer a prompt: the program leaves the prompt's loop for
        another, so the call waits the whole 600 frames (about 10 s)."""
        result = await asyncio.to_thread(machine.press_keys, keys, look, frames, every_ms, region, zoom, delay_from,
                                         wait_for_ready, until_ready)
        if isinstance(result, list):
            return [result[0], Image(data=result[1], format="png")]
        return result

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
    async def look(region: list[int] | None = None, zoom: int | None = None, frames: int = 1,
                   every_ms: int | None = None):
        """Look at the screen: a 560x384 PNG: each Apple pixel (280x192) is drawn 2x2; regions are in Apple pixels
        (x 0-279, y 0-191), so halve coordinates read off a 560x384 screenshot.
        To look closer, give region [x, y, w, h] (Apple pixels) and/or zoom (1-8, default 2):
        the result is that part of the screen, each Apple pixel drawn zoom x zoom. A 40-column text character cell
        is 7x8 Apple pixels. The image may not exceed 1120x768. At zoom 2 or more every dot of the 560-dot line is shown (Double Hi-Res,
        80-column text); zoom 1 shows every other one. To watch something change, give frames (2-16) and
        every_ms (1-1000, required with frames): it takes frames screenshots every_ms of emulated time apart and
        returns one contact sheet (4 tiles per row, 2 pixels of grey between them, region/zoom applying to each
        tile, the whole sheet within 1280x1024), preceded by the tiles' times in ms from the first. This lets
        that much time pass. Use a small region and zoom for long strips."""
        result = await asyncio.to_thread(machine.look, region, zoom, frames, every_ms)
        if isinstance(result, list):
            return [result[0], Image(data=result[1], format="png")]
        return Image(data=result, format="png")

    @mcp.tool()
    async def wait_until_change(timeout_ms: int, region: list[int] | None = None, look: bool = True,
                                since: str = "now"):
        """Let emulated time pass until the screen changes, for at most timeout_ms (1-60000), then return at once.
        Use it instead of guessing a wait: after typing a command or starting a program, it returns as soon as
        something is drawn. It compares the screen as it is now with each later frame, and stops at the first frame
        in which any pixel differs. With since "last_input" it compares with the screen as it was just after your
        last input (press_key, press_keys, type_text or set_joystick) was sent instead, so a change that already
        happened during that call (a prompt drawn while the key was being read) counts at once rather than being
        missed. region [x, y, w, h] (Apple pixels, x 0-279, y 0-191: halve coordinates read off a 560x384
        screenshot) only selects which part is watched; changes elsewhere are ignored. The result is "changed after
        N ms" or "no change in N ms", N being emulated time since the call (with since "last_input": since that
        input, and the text says so); running out of time is not an error. In a stepped session it steps frame by
        frame, so a long timeout takes real time to run. With look true (the default) it also
        returns the whole screen at that moment as a 560x384 PNG (never cropped to region). In a realtime session
        the game keeps running while you wait. Anything that flickers in the watched region (a blinking cursor, an
        animation) counts as a change at once, so choose a region that excludes it."""
        result = await asyncio.to_thread(machine.wait_until_change, timeout_ms, region, look, since)
        if isinstance(result, list):
            return [result[0], Image(data=result[1], format="png")]
        return result

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
    async def export_disk(drive: int, path: str) -> str:
        """Save the floppy currently in the disk drive (drive 1 or 2) to a new image file, including everything the
        machine has written to it so far. The copy has the same format as the disk (.dsk, .po, .nib, .woz). path is
        where to write it (a relative path is taken from the server's working directory; paths starting with two
        backslashes or two slashes are rejected). The directory must exist and the file must not: it never
        overwrites anything. The disk stays in the drive."""
        return await asyncio.to_thread(machine.export_disk, drive, path)

    @mcp.tool()
    async def set_speed(speed: str | None = None, disk_boost: bool | None = None) -> str:
        """Change how fast the machine runs (realtime sessions only). speed is "authentic" (the real 1.02 MHz, the
        same as 10), "max" (unthrottled, the same as 40) or 0-40: 0-9 give 0.5x to 0.95x, 10-39 give speed/10 (20
        is 2x). disk_boost false keeps the pace constant while a disk drive runs; true (the default) runs at full
        speed whenever a drive motor is on. Anything left out keeps its value; with no arguments it just reports
        the current speed. wait counts real time, so at a higher speed more of the machine's time passes."""
        return await asyncio.to_thread(machine.set_speed, speed, disk_boost)

    if not args.realtime:   # checkpoints are for stepped sessions only (user ruling 2026-10-20)
        @mcp.tool()
        async def checkpoint(name: str | None = None) -> str:
            """Save the whole machine (memory, CPU, video, sound, joystick, and the floppies in both drives) as a
            checkpoint you can return to with restore_checkpoint, e.g. before a risky jump. name is an optional label
            (up to 64 printable characters). Returns the checkpoint id (cp-1, cp-2, ...) and the emulated time. Up to 256
            checkpoints; they last as long as this session. Only in a stepped session."""
            return await asyncio.to_thread(machine.checkpoint, name)

        @mcp.tool()
        async def restore_checkpoint(checkpoint_id: str) -> str:
            """Put the machine and its floppies back exactly as they were at a checkpoint (an id such as cp-1 from
            checkpoint or list_checkpoints). Input still waiting to take effect is dropped, and the time continues
            from the checkpoint's. Replaying the same input from the same checkpoint gives exactly the same result,
            so you can retry a manoeuvre with different timing. The checkpoint is kept; restore it as often as you
            like. Only in a stepped session. If a restore itself fails, the session cannot be used any further."""
            return await asyncio.to_thread(machine.restore_checkpoint, checkpoint_id)

        @mcp.tool()
        async def list_checkpoints() -> str:
            """List this session's checkpoints, oldest first: id, name ("-" if none) and emulated time."""
            return await asyncio.to_thread(machine.list_checkpoints)

        @mcp.tool()
        async def delete_checkpoint(checkpoint_id: str) -> str:
            """Delete a checkpoint you no longer need (an id such as cp-1)."""
            return await asyncio.to_thread(machine.delete_checkpoint, checkpoint_id)

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
        _writer.close()
    return 0
