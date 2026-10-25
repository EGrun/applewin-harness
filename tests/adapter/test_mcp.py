# SPDX-License-Identifier: MIT
"""MCP adapter over stdio (spec: docs/specs/m5.md, pass conditions 1 and 3; plan Review Focus 1, 2, 3 and 5).

Run: python -m unittest discover -s tests/adapter -v   (HARNESS_EXE selects the exe; default bin/x64/Debug).
Needs `pip install -r adapter/requirements.txt`. The adapter is launched as `python -m applewin_mcp` through the MCP
SDK's stdio client with PYTHONPATH=adapter/. Contract: the adapter writes the line "harness pid <N>" to stderr once
the harness is running.
"""

import asyncio
import base64
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "adapter"))

from applewin_mcp.harness_client import REPO_ROOT, exe_path  # noqa: E402

sys.path.insert(0, os.path.join(REPO_ROOT, "tests", "python"))
from test_disks import UNIQUE_NAME, build_fixture  # noqa: E402  (the disk-swap fixture builder)

from mcp import ClientSession, StdioServerParameters  # noqa: E402
from mcp.client.stdio import stdio_client  # noqa: E402

DOS_IMAGE = os.path.join(REPO_ROOT, "external", "AppleWin", "bin", "DOS 3.3 System Master - 680-0210-A.dsk")
ADAPTER_DIR = os.path.join(REPO_ROOT, "adapter")
CALL_TIMEOUT = 120.0
TEST_TIMEOUT = 300.0

CHECKPOINT_TOOLS = ["checkpoint", "delete_checkpoint", "list_checkpoints", "restore_checkpoint"]   # stepped only
ALL_TOOLS = sorted(["eject_disk", "export_disk", "insert_disk", "listen", "look", "press_key", "press_keys",
                    "read_screen", "set_joystick", "set_speed", "type_text", "wait", "wait_until_change"]
                   + CHECKPOINT_TOOLS)
NO_TEXT_TOOLS = [t for t in ALL_TOOLS if t != "read_screen"]
REALTIME_TOOLS = [t for t in ALL_TOOLS if t not in CHECKPOINT_TOOLS]


def server_params(text_screen=True, extra_args=()):
    env = dict(os.environ)
    env["PYTHONPATH"] = ADAPTER_DIR + os.pathsep + env.get("PYTHONPATH", "")
    args = ["-m", "applewin_mcp", "--exe", os.path.abspath(exe_path()), "--headless", "--disk", DOS_IMAGE]
    if text_screen:
        args.append("--text-screen")
    args += list(extra_args)
    return StdioServerParameters(command=sys.executable, args=args, env=env, cwd=REPO_ROOT)


def pid_alive(pid):
    out = subprocess.run(["tasklist", "/FI", "PID eq {}".format(pid), "/FO", "CSV", "/NH"], stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True).stdout
    return '"{}"'.format(pid) in out


def text_of(result):
    return "\n".join(c.text for c in result.content if c.type == "text")


def png_size(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    return struct.unpack(">II", data[16:24])


def decode_png_rgb(data):
    """(width, height, rgb) of an 8-bit RGB (colour type 2) PNG; fails clearly on anything else."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, header = 8, b"", None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length
    width, height, depth, ctype, _, _, interlace = header
    assert (depth, ctype, interlace) == (8, 2, 0), "PNG is not 8-bit RGB, non-interlaced: {}".format(header)
    raw = zlib.decompress(idat)
    stride = width * 3
    out, prev = [], bytearray(stride)
    for y in range(height):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            else:
                assert ft == 0, "bad PNG filter type {}".format(ft)
        out.append(bytes(line))
        prev = line
    return width, height, b"".join(out)


@contextlib.asynccontextmanager
async def mcp_session(errlog, text_screen=True, extra_args=()):
    async with stdio_client(server_params(text_screen, extra_args), errlog=errlog) as (read, write):
        async with ClientSession(read, write) as session:
            await asyncio.wait_for(session.initialize(), CALL_TIMEOUT)
            yield session


async def call(session, tool, **arguments):   # not "name": checkpoint takes a name argument
    return await asyncio.wait_for(session.call_tool(tool, arguments), CALL_TIMEOUT)


FIXTURE_DIR = None   # a tempfile directory for the module, removed in tearDownModule
FIXTURE_DISK = None  # a DOS 3.3 disk holding the program UNIQUE_NAME (built once, with a pipe session)


def setUpModule():
    global FIXTURE_DIR, FIXTURE_DISK
    FIXTURE_DIR = tempfile.mkdtemp(prefix="awh-m5-adapter-")
    FIXTURE_DISK = os.path.join(FIXTURE_DIR, "fixture.dsk")
    try:
        build_fixture(FIXTURE_DISK)
    except BaseException:
        shutil.rmtree(FIXTURE_DIR, True)
        raise


def tearDownModule():
    shutil.rmtree(FIXTURE_DIR, True)


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


async def m5_end_to_end(test, s):
    """Pass condition 1 of M5 (and Review Focus 3: no wait between type_text and the screen reads)."""
    waited = await call(s, "wait", ms=12000)   # boot DOS 3.3
    test.assertFalse(waited.is_error, text_of(waited))
    for line in ('10 FOR I = 1 TO 10', '20 PRINT I', '30 NEXT I'):
        typed = await call(s, "type_text", text=line + "\n")
        test.assertFalse(typed.is_error, text_of(typed))
    shown = await call(s, "read_screen")
    test.assertFalse(shown.is_error, text_of(shown))
    test.assertIn("20 PRINT I", text_of(shown))   # visible at once, no wait in between
    ran = await call(s, "type_text", text="HOME:RUN\n")
    test.assertFalse(ran.is_error, text_of(ran))
    await call(s, "wait", ms=1500)
    screen = text_of(await call(s, "read_screen"))
    lines = screen.splitlines()
    test.assertTrue(lines[0].startswith("40col"), lines[:1])
    rows = [l.strip() for l in lines[1:]]
    test.assertEqual(rows[:10], [str(n) for n in range(1, 11)], screen)
    looked = await call(s, "look")
    test.assertFalse(looked.is_error, text_of(looked))
    images = [c for c in looked.content if c.type == "image"]
    test.assertEqual(len(images), 1)
    test.assertEqual(images[0].mime_type, "image/png")
    test.assertEqual(png_size(base64.b64decode(images[0].data)), (560, 384))
    # press_key takes a numeric ASCII code (13 = RETURN), not only a string
    schema = next(t for t in (await s.list_tools()).tools if t.name == "press_key").input_schema
    test.assertIn("integer", [o.get("type") for o in schema["properties"]["key"]["anyOf"]])
    pressed = await call(s, "press_key", key=13)
    test.assertFalse(pressed.is_error, text_of(pressed))


class AdapterTestCase(unittest.TestCase):
    def setUp(self):
        self.errlog = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
        self.addCleanup(self.errlog.close)

    def run_async(self, coro_fn):
        async def guarded():
            return await asyncio.wait_for(coro_fn(), TEST_TIMEOUT)
        try:
            return asyncio.run(guarded())
        except BaseExceptionGroup as group:   # anyio task groups wrap the test's own assertion; show that one
            leaf = group
            while isinstance(leaf, BaseExceptionGroup):
                leaf = leaf.exceptions[0]
            raise leaf from None

    def stderr_text(self):
        self.errlog.flush()
        self.errlog.seek(0)
        return self.errlog.read()

    def harness_pid(self, timeout=30.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            m = re.search(r"harness pid (\d+)", self.stderr_text())
            if m:
                return int(m.group(1))
            time.sleep(0.1)
        self.fail("the adapter never logged 'harness pid N' to stderr; stderr: {!r}".format(self.stderr_text()))


class ToolListTest(AdapterTestCase):
    def tool_names(self, text_screen, extra_args=()):
        async def go():
            async with mcp_session(self.errlog, text_screen=text_screen, extra_args=extra_args) as s:
                return sorted(t.name for t in (await asyncio.wait_for(s.list_tools(), CALL_TIMEOUT)).tools)
        return self.run_async(go)

    def test_exactly_the_agent_tools_with_text_screen(self):
        self.assertEqual(self.tool_names(True), ALL_TOOLS)

    def test_look_and_press_keys_describe_the_screen_scale(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                tools = {t.name: t for t in (await asyncio.wait_for(s.list_tools(), CALL_TIMEOUT)).tools}
                for name in ("look", "press_keys"):
                    with self.subTest(tool=name):
                        self.assertIn("560x384", tools[name].description)
                        self.assertIn("280x192", tools[name].description)
        self.run_async(go)

    def test_read_screen_absent_without_text_screen(self):
        self.assertEqual(self.tool_names(False), NO_TEXT_TOOLS)

    def test_checkpoint_tools_absent_in_realtime(self):
        """Checkpoints are for stepped sessions only (user ruling 2026-10-20): a realtime session does not list them,
        and calling one is an error."""
        self.assertEqual(self.tool_names(True, ["--realtime"]), REALTIME_TOOLS)

        async def go():
            async with mcp_session(self.errlog, extra_args=["--realtime"]) as s:
                for name in CHECKPOINT_TOOLS:
                    with self.subTest(tool=name):
                        args = {"checkpoint_id": "cp-1"} if name in ("restore_checkpoint", "delete_checkpoint") else {}
                        result = await call(s, name, **args)
                        self.assertTrue(result.is_error)
        self.run_async(go)

    def test_no_operator_method_is_reachable(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                for name in ("step_frames", "get_state_checksum", "create_session", "close_session", "pause",
                             "resume"):
                    with self.subTest(tool=name):
                        result = await call(s, name)
                        self.assertTrue(result.is_error)
                        self.assertNotIn("not implemented", text_of(result).lower())
        self.run_async(go)


class EndToEndTest(AdapterTestCase):
    def test_boot_type_run_read_and_look(self):
        """Pass condition 1 (and Review Focus 3: no wait between type_text and the screen reads)."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await m5_end_to_end(self, s)
        self.run_async(go)

    def test_input_never_locks_up_when_the_program_stops_reading_keys(self):
        """A program that does not read the keyboard (Applesoft's Ctrl-C check peeks $C000 without clearing the
        strobe) must not block later input: press_key replaces an unread key, an unread type_text fails and is
        dropped, and RESET still gets through (D10, "Keys and the strobe")."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                for text in ("10 GOTO 10\n", "RUN\n"):
                    typed = await call(s, "type_text", text=text)
                    self.assertFalse(typed.is_error, text_of(typed))
                for key in ("A", "B", "LEFT"):   # "A" stays unread; the others replace it
                    pressed = await call(s, "press_key", key=key)
                    self.assertFalse(pressed.is_error, "{}: {}".format(key, text_of(pressed)))
                typed = await call(s, "type_text", text="X")
                self.assertTrue(typed.is_error)
                self.assertIn("keyboard", text_of(typed))
                reset = await call(s, "press_key", key="RESET")
                self.assertFalse(reset.is_error, text_of(reset))
                await call(s, "wait", ms=500)
                rows = [r.strip() for r in text_of(await call(s, "read_screen")).splitlines()[1:] if r.strip()]
                self.assertTrue(rows[-1].startswith("]"), rows[-3:])   # back at the prompt (the cursor may follow)
        self.run_async(go)

    def test_look_region_and_zoom(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)

                async def size(**args):
                    r = await call(s, "look", **args)
                    self.assertFalse(r.is_error, text_of(r))
                    images = [c for c in r.content if c.type == "image"]
                    self.assertEqual(len(images), 1)
                    return png_size(base64.b64decode(images[0].data))
                self.assertEqual(await size(region=[0, 0, 40, 8], zoom=4), (160, 32))
                self.assertEqual(await size(zoom=1), (280, 192))
                self.assertEqual(await size(), (560, 384))
                bad = await call(s, "look", region=[270, 0, 20, 10])
                self.assertTrue(bad.is_error)
                self.assertIn("280x192", text_of(bad))
        self.run_async(go)

    def test_look_zoom_2_matches_plain_look_in_80_columns(self):
        """Zoom 2 over the whole screen is the full 560x384 frame, dot for dot (80-column text uses every dot)."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)
                for text in ("PR#3\n", "HOME:PRINT \"MIXED Il1|\"\n"):
                    typed = await call(s, "type_text", text=text)
                    self.assertFalse(typed.is_error, text_of(typed))
                await call(s, "wait", ms=500)

                async def image(**args):
                    r = await call(s, "look", **args)
                    self.assertFalse(r.is_error, text_of(r))
                    return decode_png_rgb(base64.b64decode([c for c in r.content if c.type == "image"][0].data))
                plain, zoomed = await image(), await image(zoom=2)
                self.assertEqual(plain[:2], (560, 384))
                self.assertEqual(zoomed[:2], (560, 384))
                self.assertTrue(plain[2] == zoomed[2], "adapter zoom=2 differs from the harness PNG")
        self.run_async(go)

    def test_strip_parameter_errors(self):
        """Everything is validated before the first tile, so a rejected call lets no time pass."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)
                for text in ("10 GOTO 10\n", "RUN\n"):
                    await call(s, "type_text", text=text)

                async def png(**args):
                    r = await call(s, "look", **args)
                    self.assertFalse(r.is_error, text_of(r))
                    return base64.b64decode([c for c in r.content if c.type == "image"][0].data)
                before = await png(zoom=1)
                for args in ({"frames": 3}, {"frames": 17, "every_ms": 10}, {"frames": 0, "every_ms": 10},
                             {"frames": 2, "every_ms": 1001}, {"frames": 8, "every_ms": 10, "zoom": 8},
                             {"frames": 1, "every_ms": 5000}):
                    with self.subTest(args=args):
                        r = await call(s, "look", **args)
                        self.assertTrue(r.is_error, args)
                self.assertEqual(await png(zoom=1), before)
        self.run_async(go)

    def test_strip_tile_times_are_exact_in_stepped_mode(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)
                r = await call(s, "look", frames=4, every_ms=100, zoom=1)
                self.assertFalse(r.is_error, text_of(r))
                self.assertEqual([c.type for c in r.content], ["text", "image"])
                m = re.fullmatch(r"tile times: ([\d., ]+) ms", r.content[0].text)
                self.assertTrue(m, r.content[0].text)
                times = [float(v) for v in m.group(1).split(", ")]
                self.assertEqual(len(times), 4)
                self.assertEqual(times[0], 0.0)
                for i, t in enumerate(times):
                    self.assertAlmostEqual(t, 100.0 * i, delta=16.7)
                self.assertTrue(all(a < b for a, b in zip(times, times[1:])), times)
                self.assertEqual(png_size(base64.b64decode(r.content[1].data)), (4 * 280 + 3 * 2, 192))
        self.run_async(go)

    def test_strip_realtime_smoke(self):
        async def go():
            async with mcp_session(self.errlog, extra_args=["--realtime"]) as s:
                await call(s, "wait", ms=12000)
                r = await call(s, "look", frames=2, every_ms=200, zoom=1)
                self.assertFalse(r.is_error, text_of(r))
                m = re.fullmatch(r"tile times: ([\d., ]+) ms", r.content[0].text)
                self.assertTrue(m, r.content[0].text)
                times = [float(v) for v in m.group(1).split(", ")]
                self.assertEqual(len(times), 2)
                self.assertLess(times[0], times[1])
                self.assertGreaterEqual(times[1], 150.0)
        self.run_async(go)

    def test_press_keys_strip_starts_at_the_last_key(self):
        """press_keys with frames: the strip's first tile is taken as soon as the last key is read and released,
        and tile times count from the last key's press."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                screen = text_of(await call(s, "read_screen"))
                for bad in ({"look": True, "frames": 2, "every_ms": 10}, {"frames": 2},
                            {"frames": 17, "every_ms": 10}, {"region": [0, 0, 10, 10]},
                            {"look": True, "region": [270, 0, 20, 10]}, {"look": True, "zoom": 9}):
                    with self.subTest(**bad):
                        r = await call(s, "press_keys", keys=["Z"], **bad)
                        self.assertTrue(r.is_error, bad)
                self.assertEqual(text_of(await call(s, "read_screen")), screen)   # rejected before any key went in
                r = await call(s, "press_keys", keys=["A"], frames=3, every_ms=100, zoom=1)
                self.assertFalse(r.is_error, text_of(r))
                self.assertEqual([c.type for c in r.content], ["text", "image"])
                lines = r.content[0].text.splitlines()
                self.assertRegex(lines[0], r"^1\. A  applied 0\.0 ms  read \d+\.\d ms \(latency \d+\.\d ms\)$")
                m = re.fullmatch(r"tile times from the last key's press: ([\d.]+), ([\d.]+), ([\d.]+) ms", lines[-1])
                self.assertTrue(m, lines)
                t = [float(v) for v in m.groups()]
                self.assertTrue(50 <= t[0] < 50 + 2 * 16.7, t)   # the default 50 ms hold, then the next frame
                for a, b in zip(t, t[1:]):
                    self.assertTrue(99 <= b - a <= 117, t)
                self.assertEqual(png_size(base64.b64decode(r.content[1].data)), (3 * 280 + 2 * 2, 192))
        self.run_async(go)

    def test_screen_tools_say_to_halve_screenshot_coordinates(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                tools = {t.name: t.description for t in (await s.list_tools()).tools}
                for name in ("look", "press_keys", "wait_until_change"):
                    with self.subTest(tool=name):
                        self.assertIn("halve", tools[name])
        self.run_async(go)

    def test_press_keys_look_returns_report_then_screen(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)
                r = await call(s, "press_keys", keys=["P"], look=True)
                self.assertFalse(r.is_error, text_of(r))
                self.assertEqual([c.type for c in r.content], ["text", "image"])
                self.assertTrue(r.content[0].text.startswith("1. P  applied 0.0 ms"), r.content[0].text)
                self.assertEqual(png_size(base64.b64decode(r.content[1].data)), (560, 384))
        self.run_async(go)

    def test_press_keys_sends_a_timed_sequence_and_reports_each_key(self):
        """press_keys (D10, "Key sequences"): one call, each key's applied and read time; a program that reads the
        keyboard without clearing the strobe still reads the keys; a sequence the program stops reading is a tool
        error that reports the keys pressed, and leaves the machine usable."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                sent = await call(s, "press_keys", keys=["P", "R", {"key": "I", "delay_ms": 100}, "N", "T"])
                self.assertFalse(sent.is_error, text_of(sent))
                lines = text_of(sent).splitlines()
                self.assertEqual(len(lines), 5, lines)
                times = [tuple(float(v) for v in re.findall(r"(?:applied|read) (\d+(?:\.\d+)?) ms", l)) for l in lines]
                self.assertEqual(times[0][0], 0.0)
                self.assertTrue(all(applied <= read for applied, read in times), lines)   # a read can be on the apply cycle
                self.assertGreaterEqual(times[2][0] - times[1][0], 100.0)
                self.assertIn("]PRINT", text_of(await call(s, "read_screen")))

                await call(s, "type_text", text="\n10 GOTO 10\n")
                await call(s, "type_text", text="RUN\n")   # Applesoft's Ctrl-C check reads $C000, never clears it
                polled = await call(s, "press_keys", keys=["LEFT", "LEFT", "Q"])
                self.assertFalse(polled.is_error, text_of(polled))
                self.assertEqual(len(text_of(polled).splitlines()), 3)
                await call(s, "press_key", key="RESET")
                await call(s, "press_key", key="RETURN")   # replaces the unread "Q" before BASIC's prompt takes it

                # JMP $0300 for ever: nothing reads the keyboard.
                await call(s, "type_text", text="POKE 768,76:POKE 769,0:POKE 770,3:CALL 768\n")
                await call(s, "wait", ms=500)   # until CALL 768: BASIC's Ctrl-C check reads $C000 between statements
                stuck = await call(s, "press_keys", keys=["A", "B"])
                self.assertTrue(stuck.is_error, text_of(stuck) + "\n" + text_of(await call(s, "read_screen")))
                report = text_of(stuck)
                self.assertIn("keyboard", report)
                self.assertRegex(report, r"1\. A\s+applied 0\.0 ms\s+never read")
                self.assertRegex(report, r"2\. B\s+not pressed")
                reset = await call(s, "press_key", key="RESET")
                self.assertFalse(reset.is_error, text_of(reset))
        self.run_async(go)

    def test_press_keys_with_joystick_entries(self):
        """A joystick entry is reported as a "joystick" line with no read time; key lines are unchanged."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                mixed = await call(s, "press_keys", keys=[{"joystick": {"x": 0}, "duration_ms": 100}, "A"])
                self.assertFalse(mixed.is_error, text_of(mixed))
                lines = text_of(mixed).splitlines()
                self.assertEqual(len(lines), 2, lines)
                self.assertRegex(lines[0], r"^1\. joystick\s+applied 0\.0 ms$")
                self.assertRegex(lines[1], r"^2\. A\s+applied \d+(\.\d+)? ms\s+read \d+(\.\d+)? ms \(latency \d+\.\d ms\)$")
                alone = await call(s, "press_keys", keys=[{"joystick": {"x": 0}, "duration_ms": 100}])
                self.assertFalse(alone.is_error, text_of(alone))
                self.assertRegex(text_of(alone), r"^1\. joystick\s+applied 0\.0 ms$")
        self.run_async(go)

    def test_stdout_is_clean_mcp_only(self):
        """Review Focus 1: a stray byte on stdout would break the session; a round trip must work."""
        async def go():
            async with mcp_session(self.errlog) as s:
                result = await call(s, "wait", ms=100)
                self.assertFalse(result.is_error, text_of(result))
                result = await call(s, "read_screen")
                self.assertFalse(result.is_error, text_of(result))
        self.run_async(go)

    def test_long_slow_typing_is_not_a_stall(self):
        """120 keys at 100 ms is 720 frames: longer than the 600-frame stall cap, but the keyboard is being read."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                typed = await call(s, "type_text", text="10 REM " + "A" * 113 + chr(10), inter_key_delay_ms=100)
                self.assertFalse(typed.is_error, text_of(typed))
                self.assertIn("AAAAAAAA", text_of(await call(s, "read_screen")))
        self.run_async(go)

    def test_wait_and_listen_ranges(self):
        """wait is 1-60000 ms, listen 0-60000 ms. The stub's NotImplementedError reaches the client only as the opaque
        text 'Error executing tool <name>', so an error alone cannot tell stub from validation: the in-range calls
        (which the stub fails) are what pin the behavior down."""
        async def go():
            async with mcp_session(self.errlog) as s:
                for name, ms in (("wait", 1), ("wait", 60000), ("listen", 0), ("listen", 60000)):
                    with self.subTest(name=name, ms=ms, kind="in range"):
                        result = await call(s, name, ms=ms)
                        self.assertFalse(result.is_error, text_of(result))
                for name, ms in (("wait", 0), ("wait", 60001), ("listen", -1), ("listen", 60001)):
                    with self.subTest(name=name, ms=ms, kind="out of range"):
                        result = await call(s, name, ms=ms)
                        self.assertTrue(result.is_error, "{}(ms={}) must be rejected".format(name, ms))
        self.run_async(go)

class WaitUntilChangeTest(AdapterTestCase):
    JMP_LOOP = "POKE 768,76:POKE 769,0:POKE 770,3:CALL 768\n"

    async def start_loop(self, s):
        await call(s, "wait", ms=12000)   # boot DOS 3.3
        await call(s, "type_text", text=self.JMP_LOOP)
        await call(s, "wait", ms=500)

    def check_no_change(self, r, low, high):
        self.assertFalse(r.is_error, text_of(r))
        m = re.fullmatch(r"no change in (\d+) ms", r.content[0].text)
        self.assertTrue(m, r.content[0].text)
        self.assertTrue(low <= int(m.group(1)) <= high, r.content[0].text)

    def test_wait_until_change_sees_printed_text(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                await call(s, "type_text", text='10 FOR I=1 TO 4000:NEXT:PRINT "X"\n')
                await call(s, "type_text", text="RUN\n")
                await call(s, "wait", ms=100)   # the echo of RUN is on the screen before the reference is taken
                before = text_of(await call(s, "read_screen")).splitlines()
                self.assertNotIn("X", [l.strip() for l in before])
                r = await call(s, "wait_until_change", timeout_ms=5000)
                self.assertFalse(r.is_error, text_of(r))
                self.assertEqual([c.type for c in r.content], ["text", "image"])
                m = re.fullmatch(r"changed after (\d+) ms", r.content[0].text)
                self.assertTrue(m, r.content[0].text)
                self.assertGreater(int(m.group(1)), 50)   # the program's own PRINT, well after the reference
                self.assertEqual(png_size(base64.b64decode(r.content[1].data)), (560, 384))
                self.assertIn("X", [l.strip() for l in text_of(await call(s, "read_screen")).splitlines()])
        self.run_async(go)

    def test_wait_until_change_text_only_without_look(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await self.start_loop(s)
                r = await call(s, "wait_until_change", timeout_ms=100, look=False)
                self.assertEqual([c.type for c in r.content], ["text"])
                self.check_no_change(r, 100, 120)
        self.run_async(go)

    def test_wait_until_change_times_out_without_error(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await self.start_loop(s)
                r = await call(s, "wait_until_change", timeout_ms=300)
                self.check_no_change(r, 300, 319)
                self.assertEqual([c.type for c in r.content], ["text", "image"])
        self.run_async(go)

    def test_wait_until_change_tiny_timeout(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await self.start_loop(s)
                self.check_no_change(await call(s, "wait_until_change", timeout_ms=1), 0, 17)
        self.run_async(go)

    def test_wait_until_change_region_only_watches(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await self.start_loop(s)
                r = await call(s, "wait_until_change", timeout_ms=200, region=[0, 0, 280, 8])
                self.check_no_change(r, 200, 220)
                self.assertEqual(png_size(base64.b64decode(r.content[1].data)), (560, 384))
        self.run_async(go)

    def test_wait_until_change_unsubscribes(self):
        """After a realtime wait the harness must have stopped sending frame_ready: observed on the agent connection
        of an in-process Machine (the MCP client cannot see notifications)."""
        from applewin_mcp import server
        args = server.parse_args(["--exe", os.path.abspath(exe_path()), "--headless", "--disk", DOS_IMAGE,
                                  "--realtime"])
        machine = server.Machine(args)
        self.addCleanup(machine.shutdown)
        machine.wait(12000)   # boot DOS 3.3
        machine.type_text("POKE 768,76:POKE 769,0:POKE 770,3:CALL 768\n", 0)
        machine.wait(500)
        for _ in range(2):
            text = machine.wait_until_change(200, None, False)
            self.assertRegex(text, r"no change in \d+ ms")
        while True:   # replies and notifications are in order: what is queued now predates the unsubscribe
            try:
                machine.agent.next_notification(timeout=0)
            except TimeoutError:
                break
        deadline = time.monotonic() + 0.7
        late = []
        while time.monotonic() < deadline:
            try:
                note = machine.agent.next_notification(timeout=0.1)
            except TimeoutError:
                continue
            if note.get("method") == "frame_ready":
                late.append(note)
        self.assertEqual(late, [], "frame_ready still arriving after wait_until_change")

    def test_wait_until_change_sees_printed_text_realtime(self):
        async def go():
            async with mcp_session(self.errlog, extra_args=["--realtime"]) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                await call(s, "type_text", text='10 FOR I=1 TO 300:NEXT:PRINT "X"\n')
                await call(s, "type_text", text="RUN\n")
                r = await call(s, "wait_until_change", timeout_ms=5000)
                self.assertFalse(r.is_error, text_of(r))
                self.assertEqual([c.type for c in r.content], ["text", "image"])
                m = re.fullmatch(r"changed after (\d+) ms", r.content[0].text)
                self.assertTrue(m, r.content[0].text)
                self.assertGreater(int(m.group(1)), 0)
                self.assertEqual(png_size(base64.b64decode(r.content[1].data)), (560, 384))
        self.run_async(go)

    def test_wait_until_change_since_last_input_sees_a_change_made_during_the_input(self):
        """The change happens during press_key (its settle frames), before wait_until_change starts: the default
        reference misses it, since="last_input" compares with the screen from just after the key was sent."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                await call(s, "type_text", text="POKE 768,76:POKE 769,0:POKE 770,3\n")
                await call(s, "type_text", text="10 GET A$:HOME:PRINT A$:CALL 768\n")
                await call(s, "type_text", text="RUN\n")
                await call(s, "wait", ms=200)
                pressed = await call(s, "press_key", key="Q")   # HOME, PRINT, then a JMP loop: a static screen
                self.assertFalse(pressed.is_error, text_of(pressed))
                await call(s, "wait", ms=300)
                self.check_no_change(await call(s, "wait_until_change", timeout_ms=200), 200, 220)
                r = await call(s, "wait_until_change", timeout_ms=200, since="last_input")
                self.assertFalse(r.is_error, text_of(r))
                m = re.fullmatch(r"changed after (\d+) ms \(since the last input\)", r.content[0].text)
                self.assertTrue(m, r.content[0].text)
                self.assertGreaterEqual(int(m.group(1)), 300)   # counted from the key, not from this call
                self.assertEqual(png_size(base64.b64decode(r.content[1].data)), (560, 384))
        self.run_async(go)

    def test_wait_until_change_since_needs_an_input_and_a_known_value(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                r = await call(s, "wait_until_change", timeout_ms=10, since="last_input")
                self.assertTrue(r.is_error)
                self.assertIn("no input", text_of(r))
                r = await call(s, "wait_until_change", timeout_ms=10, since="yesterday")
                self.assertTrue(r.is_error)
                self.assertIn("since", text_of(r))
        self.run_async(go)

    def test_wait_until_change_ranges_and_region(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                for kwargs in ({"timeout_ms": 0}, {"timeout_ms": 60001},
                               {"timeout_ms": 100, "region": [0, 0, 281, 8]}):
                    with self.subTest(**kwargs):
                        r = await call(s, "wait_until_change", **kwargs)
                        self.assertTrue(r.is_error, kwargs)
        self.run_async(go)


class ReadAnchoredTimingTest(AdapterTestCase):
    """press_keys delay_from "read" (D10, "Read-anchored delays") against a 6502 loop that polls $C000 every 92,240
    cycles (about 90.4 ms) without clearing the strobe, like a slowly polling title screen."""

    POLL_LOOP = ":".join("POKE {},{}".format(768 + i, b) for i, b in enumerate(
        [173, 0, 192, 162, 72, 160, 255, 136, 208, 253, 202, 208, 248, 76, 0, 3])) + "\n"

    async def start_loop(self, s):
        await call(s, "wait", ms=12000)   # boot DOS 3.3
        await call(s, "type_text", text=self.POLL_LOOP)
        await call(s, "type_text", text="CALL 768\n")
        await call(s, "wait", ms=500)

    @staticmethod
    def times(text):
        """[(applied ms, read ms or None, latency ms or None)] per report line."""
        rows = []
        for line in text.splitlines():
            m = re.search(r"applied ([\d.]+) ms(?:\s+read ([\d.]+) ms \(latency ([\d.]+) ms\))?", line)
            rows.append((float(m.group(1)), None if m.group(2) is None else float(m.group(2)),
                         None if m.group(3) is None else float(m.group(3))))
        return rows

    def test_the_gap_the_program_sees_does_not_depend_on_the_phase(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await self.start_loop(s)
                gaps = {"applied": set(), "read": set()}
                for offset in range(0, 100, 20):   # the phase at which the keys arrive
                    await call(s, "wait", ms=offset)
                    for delay_from in ("applied", "read"):
                        r = await call(s, "press_keys", keys=["A", {"key": "B", "delay_ms": 200}],
                                       delay_from=delay_from)
                        self.assertFalse(r.is_error, text_of(r))
                        (_, read_a, _), (_, read_b, _) = self.times(text_of(r))
                        gaps[delay_from].add(round(read_b - read_a, 1))
                self.assertGreater(len(gaps["applied"]), 1)   # from the apply: it flips by a whole loop period
                self.assertEqual(len(gaps["read"]), 1, gaps)    # from the read: it never does
                self.assertAlmostEqual(next(iter(gaps["read"])), 3 * 92240 * 1000.0 / 1020484.5, delta=0.2)
        self.run_async(go)

    def test_the_report_shows_the_latency_and_a_per_entry_override(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await self.start_loop(s)
                r = await call(s, "press_keys", delay_from="read",
                               keys=["A", {"key": "B", "delay_ms": 100}, {"key": "C", "delay_ms": 100,
                                                                       "delay_from": "applied"}])
                self.assertFalse(r.is_error, text_of(r))
                rows = self.times(text_of(r))
                for applied, read, latency in rows:
                    self.assertAlmostEqual(latency, read - applied, delta=0.15)
                # B counts from A's read, C (delay_from applied) from B's apply
                self.assertAlmostEqual(rows[1][0], rows[0][1] + 100, delta=0.15)
                self.assertAlmostEqual(rows[2][0], rows[1][0] + 100, delta=0.15)
        self.run_async(go)

    def test_a_bad_delay_from_is_an_error_and_presses_nothing(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)
                screen = text_of(await call(s, "read_screen"))
                for kwargs in ({"keys": ["Z"], "delay_from": "later"},
                               {"keys": [{"key": "Z", "delay_from": "now"}]},
                               {"keys": [{"key": "Z", "delay_from": "read", "wait_for_read": False}]}):
                    with self.subTest(**kwargs):
                        r = await call(s, "press_keys", **kwargs)
                        self.assertTrue(r.is_error)
                self.assertEqual(text_of(await call(s, "read_screen")), screen)
        self.run_async(go)

    def test_press_keys_describes_delay_from(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                tools = {t.name: t.description for t in (await s.list_tools()).tools}
                self.assertIn("delay_from", tools["press_keys"])
                self.assertIn("latency", tools["press_keys"])
        self.run_async(go)



class ReadyInputTest(AdapterTestCase):
    """press_keys wait_for_ready (D10, "Ready input") against a 6502 program that behaves like a turn-based BASIC game:
    idle, it polls $C000 from two places 12.5 ms apart; a key it finds is taken (strobe cleared) and starts a 380 ms
    "move" in which it polls from a third place and ignores keys; after the move for P it asks a question, a tight
    read loop that takes keys until RETURN and then makes another move before it is idle again."""

    PROGRAM = [
        (0x300, [0xAD, 0x00, 0xC0, 0x30, 0x11, 0x20, 0x60, 0x03, 0xAD, 0x00, 0xC0, 0x30, 0x09, 0x20, 0x60, 0x03,
                 0x4C, 0x00, 0x03]),
        (0x316, [0x8D, 0x10, 0xC0, 0x8D, 0x81, 0x03, 0xA9, 0x1E, 0x8D, 0x80, 0x03, 0x20, 0x60, 0x03, 0xAD, 0x00, 0xC0,
                 0xCE, 0x80, 0x03, 0xD0, 0xF5, 0xAD, 0x81, 0x03, 0xC9, 0xD0, 0xF0, 0x05, 0x4C, 0x00, 0x03]),
        (0x338, [0xAD, 0x00, 0xC0, 0x10, 0xFB, 0xAD, 0x00, 0xC0, 0x8D, 0x10, 0xC0, 0xC9, 0x8D, 0xD0, 0xF1, 0x4C, 0x19,
                 0x03]),
        (0x360, [0xA2, 0x0A, 0xA0, 0xFF, 0x88, 0xD0, 0xFD, 0xCA, 0xD0, 0xF8, 0x60]),
    ]

    async def start_game(self, s):
        await call(s, "wait", ms=12000)   # boot DOS 3.3
        pokes = ["POKE {},{}".format(address + i, b) for address, code in self.PROGRAM for i, b in enumerate(code)]
        for i in range(0, len(pokes), 12):
            await call(s, "type_text", text=":".join(pokes[i:i + 12]) + "\n")
        await call(s, "type_text", text="CALL 768\n")
        await call(s, "wait", ms=500)

    @staticmethod
    def applied_ms(text):
        return [float(v) for v in re.findall(r"applied (\d+(?:\.\d+)?) ms", text)]

    def test_each_key_waits_until_the_program_is_idle_again(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await self.start_game(s)
                r = await call(s, "press_keys", keys=["A", "A", "A"], wait_for_ready=True)
                self.assertFalse(r.is_error, text_of(r))
                times = self.applied_ms(text_of(r))
                self.assertEqual(len(times), 3, text_of(r))
                for earlier, later in zip(times, times[1:]):
                    self.assertGreaterEqual(later - earlier, 380.0)   # a whole move later
                    self.assertLess(later - earlier, 480.0)           # and then right away
        self.run_async(go)

    def test_it_stops_at_a_prompt_and_the_prompt_can_be_answered_next(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await self.start_game(s)
                stopped = await call(s, "press_keys", keys=["A", "P", "A", "A"], wait_for_ready=True)
                self.assertTrue(stopped.is_error, text_of(stopped))
                report = text_of(stopped)
                self.assertIn("different kind of input", report)
                self.assertRegex(report, r"2\. P\s+applied [\d.]+ ms\s+read")
                self.assertRegex(report, r"3\. A\s+not pressed")
                self.assertRegex(report, r"4\. A\s+not pressed")
                answered = await call(s, "press_keys", keys=["Y", "RETURN"], wait_for_ready=True)   # at once
                self.assertFalse(answered.is_error, text_of(answered))
                again = await call(s, "press_keys", keys=["A", "A"], wait_for_ready=True)   # idle again
                self.assertFalse(again.is_error, text_of(again))
        self.run_async(go)

    def test_a_prompt_that_the_program_never_leaves_reports_not_ready(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await self.start_game(s)
                await call(s, "press_key", key="P")
                await call(s, "wait", ms=600)   # after the move, the prompt's loop is what the program polls from
                r = await call(s, "press_keys", keys=["RETURN", "A"], wait_for_ready=True)
                self.assertTrue(r.is_error, text_of(r))
                self.assertIn("not ready for the next key", text_of(r))
                self.assertRegex(text_of(r), r"2\. A\s+not pressed")
        self.run_async(go)

    def test_bad_combinations_are_errors_and_press_nothing(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)
                screen = text_of(await call(s, "read_screen"))
                for kwargs in ({"keys": ["Z", {"key": "Z", "delay_ms": 50}]}, {"keys": ["Z", "Z"], "delay_from": "read"},
                               {"keys": ["Z", {"joystick": {"x": 1}}]},
                               {"keys": ["Z", {"key": "Z", "wait_for_read": False}]}):
                    with self.subTest(**kwargs):
                        r = await call(s, "press_keys", wait_for_ready=True, **kwargs)
                        self.assertTrue(r.is_error)
                self.assertEqual(text_of(await call(s, "read_screen")), screen)
        self.run_async(go)

    def test_press_keys_describes_wait_for_ready(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                tools = {t.name: t.description for t in (await s.list_tools()).tools}
                self.assertIn("wait_for_ready", tools["press_keys"])
                self.assertIn("NOT pressed", tools["press_keys"])
        self.run_async(go)


class CheckpointTest(AdapterTestCase):
    """Stepped-session checkpoints through the adapter (user ruling 2026-10-20): the harness's checkpoint methods,
    which wrap AppleWin's own save state together with the working disks."""

    def test_save_restore_list_and_delete(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                saved = await call(s, "checkpoint", name="booted")
                self.assertFalse(saved.is_error, text_of(saved))
                self.assertRegex(text_of(saved), r"^saved cp-1 \(booted\) at \d+\.\d ms$")

                await call(s, "type_text", text='PRINT "AFTER"\n')
                self.assertIn("AFTER", text_of(await call(s, "read_screen")))
                restored = await call(s, "restore_checkpoint", checkpoint_id="cp-1")
                self.assertFalse(restored.is_error, text_of(restored))
                self.assertRegex(text_of(restored), r"^restored cp-1 \(booted\) at \d+\.\d ms$")
                await call(s, "wait", ms=100)
                self.assertNotIn("AFTER", text_of(await call(s, "read_screen")))   # the machine went back

                # Deterministic: the same input after the same checkpoint gives the same screen.
                shots = []
                for _ in range(2):
                    await call(s, "restore_checkpoint", checkpoint_id="cp-1")
                    await call(s, "press_keys", keys=["P", "R", "I", "N", "T"])
                    await call(s, "wait", ms=250)
                    shots.append(base64.b64decode((await call(s, "look")).content[0].data))
                self.assertEqual(shots[0], shots[1])

                listed = text_of(await call(s, "list_checkpoints"))
                self.assertRegex(listed, r"^cp-1  booted  \d+\.\d ms$")
                deleted = await call(s, "delete_checkpoint", checkpoint_id="cp-1")
                self.assertFalse(deleted.is_error, text_of(deleted))
                self.assertEqual(text_of(await call(s, "list_checkpoints")), "no checkpoints")
                missing = await call(s, "restore_checkpoint", checkpoint_id="cp-1")
                self.assertTrue(missing.is_error)
        self.run_async(go)

    def test_restore_forgets_the_discarded_timeline(self):
        """After a restore, since="last_input" has no reference from the thrown-away timeline."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)
                await call(s, "checkpoint")
                await call(s, "press_key", key="A")
                await call(s, "restore_checkpoint", checkpoint_id="cp-1")
                r = await call(s, "wait_until_change", timeout_ms=10, since="last_input")
                self.assertTrue(r.is_error)
                self.assertIn("no input", text_of(r))
        self.run_async(go)


class MachineUnitTest(unittest.TestCase):
    """Machine methods against a stub (no harness)."""

    def machine(self):
        from applewin_mcp import server
        m = object.__new__(server.Machine)
        m.lock = threading.Lock()
        m.stepped = False
        m.frame_ready = []
        m.collect_frames = False
        return m

    def test_whole_screen_zoom_8_is_rejected_before_a_frame_is_fetched(self):
        from mcp.server.mcpserver.exceptions import ToolError
        m = self.machine()

        def no_frame():
            raise AssertionError("frame_rgb must not be called")
        m.frame_rgb = no_frame
        with self.assertRaisesRegex(ToolError, "1120x768"):
            m._look_locked(None, 8)
        with self.assertRaisesRegex(ToolError, "1120x768"):
            m._look_locked([0, 0, 200, 100], 8)

    def stub_wait(self, pump_raises):
        from mcp.server.mcpserver.exceptions import ToolError
        m = self.machine()
        m.frame_rgb = lambda: (b"", 0)

        def rpc(conn, method, **params):
            if method == "subscribe_frames" and params["every_n_frames"] == 0:
                raise ToolError("unsubscribe failed")

        def pump(timeout=0.0):
            if pump_raises:
                raise ToolError("original failure")
            time.sleep(0.01)
        m.rpc, m.pump, m.agent = rpc, pump, object()
        return m

    def test_a_failing_unsubscribe_does_not_mask_the_original_error(self):
        from mcp.server.mcpserver.exceptions import ToolError
        with self.assertRaisesRegex(ToolError, "original failure"):
            self.stub_wait(True).wait_until_change(100, None, False)

    def test_a_failing_unsubscribe_is_an_error_when_the_wait_succeeded(self):
        from mcp.server.mcpserver.exceptions import ToolError
        with self.assertRaisesRegex(ToolError, "unsubscribe failed"):
            self.stub_wait(False).wait_until_change(1, None, False)

    def test_restore_drains_late_notices_from_the_old_timeline(self):
        """The harness sends the discarded-input notices (and any last sound events) on the agent connection, so they
        may still be unread when the restore reply arrives on the operator connection. An agent-connection call after
        the reply is a barrier: once it returns they have arrived, and the restore drains and forgets them."""
        m = self.machine()
        m.stepped = True
        m.applied, m.events, m.input_screen = {7: {"input_id": 7}}, [("sound_started", {})], (b"old", 0)
        queue, late = [], []   # late: sent by the harness, not yet read from the agent connection

        def arrive():
            queue.extend(late)
            late.clear()

        class Agent:
            def next_notification(self, timeout=0.0):
                if not queue:
                    raise TimeoutError
                return queue.pop(0)
        m.agent, m.operator = Agent(), object()

        def rpc(conn, method, **params):
            if conn is m.agent:
                arrive()   # an agent-connection reply comes after that connection's earlier notifications
            if method == "restore_checkpoint":
                late.extend([{"method": "input_applied", "params": {"input_id": 8, "discarded": True}},
                             {"method": "sound_stopped", "params": {}}])
                return {"emulated_cycles": 0}
            if method == "list_checkpoints":
                return [{"checkpoint_id": "cp-1", "name": None, "emulated_cycles": 0}]
            return {}
        m.rpc = rpc
        self.assertEqual(m.restore_checkpoint("cp-1"), "restored cp-1 at 0.0 ms")
        arrive()      # whatever was still in flight has arrived by now
        m.pump(0.0)
        self.assertEqual((m.applied, m.events, m.input_screen), ({}, [], None))

    def test_a_failed_screen_fetch_after_an_input_does_not_fail_the_input(self):
        """The input was sent: the tool still succeeds, and since="last_input" then has no reference rather than an
        older input's screen."""
        from mcp.server.mcpserver.exceptions import ToolError
        m = self.machine()
        m.input_screen = (b"old", 0)

        def no_frame():
            raise ToolError("get_frame failed")
        m.frame_rgb = no_frame
        m.rpc = lambda conn, method, **params: {}
        m.agent = object()
        self.assertEqual(m.set_joystick(10, None, None, None, 0), "joystick set; call wait to let time pass")
        self.assertIsNone(m.input_screen)


class DiskSwapTest(AdapterTestCase):
    def test_swap_disks_while_the_machine_runs(self):
        """Pass condition 5: eject and insert in drive 1, load and empty drive 2; the originals are never written."""
        master_before, fixture_before = sha256_file(DOS_IMAGE), sha256_file(FIXTURE_DISK)

        async def catalog(s, command):
            typed = await call(s, "type_text", text="HOME\n" + command + "\n")
            self.assertFalse(typed.is_error, text_of(typed))
            await call(s, "wait", ms=4000)
            # DOS 3.3 stops after 18 files and waits for a key; a space shows the end of the listing (the last page
            # still has HELLO at the top and ends with SLOT# on the master or QZXFIXTURE on the fixture).
            typed = await call(s, "type_text", text=" ")
            self.assertFalse(typed.is_error, text_of(typed))
            await call(s, "wait", ms=2000)
            return text_of(await call(s, "read_screen"))

        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                master = await catalog(s, "CATALOG")
                self.assertIn("SLOT#", master)
                self.assertNotIn(UNIQUE_NAME, master)
                ejected = await call(s, "eject_disk", drive=1)
                self.assertFalse(ejected.is_error, text_of(ejected))
                inserted = await call(s, "insert_disk", drive=1, path=FIXTURE_DISK)
                self.assertFalse(inserted.is_error, text_of(inserted))
                fixture = await catalog(s, "CATALOG")
                self.assertIn("HELLO", fixture)
                self.assertIn(UNIQUE_NAME, fixture)
                inserted = await call(s, "insert_disk", drive=2, path=DOS_IMAGE)
                self.assertFalse(inserted.is_error, text_of(inserted))
                second = await catalog(s, "CATALOG,D2")
                self.assertIn("HELLO", second)
                self.assertIn("SLOT#", second)
                self.assertNotIn(UNIQUE_NAME, second)   # drive 2 holds the master, not the fixture in drive 1
                ejected = await call(s, "eject_disk", drive=2)
                self.assertFalse(ejected.is_error, text_of(ejected))
                again = await call(s, "eject_disk", drive=2)   # empty now
                self.assertTrue(again.is_error)
        self.run_async(go)
        self.assertEqual(sha256_file(DOS_IMAGE), master_before)
        self.assertEqual(sha256_file(FIXTURE_DISK), fixture_before)

    def test_insert_disk_reports_harness_errors_as_tool_errors(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                missing = await call(s, "insert_disk", drive=2, path=os.path.join(FIXTURE_DIR, "missing.dsk"))
                self.assertTrue(missing.is_error)
                self.assertIn("disk not found", text_of(missing))   # the harness's message (the SDK prefixes ToolError text), not an opaque failure
                bad_drive = await call(s, "insert_disk", drive=3, path=FIXTURE_DISK)
                self.assertTrue(bad_drive.is_error)
                unc = await call(s, "insert_disk", drive=1, path="\\\\server\\share\\x.dsk")
                self.assertTrue(unc.is_error)
                self.assertIn("not allowed", text_of(unc))   # rejected by the adapter, never sent to the harness
        self.run_async(go)


class ExportDiskTest(AdapterTestCase):
    def test_export_disk_copies_the_working_image_and_keeps_the_original(self):
        master_before = sha256_file(DOS_IMAGE)
        dest = os.path.join(FIXTURE_DIR, "exported.dsk")
        # The adapter's cwd is REPO_ROOT; a relative path can't reach FIXTURE_DIR when %TEMP% is on another drive (CI)
        scratch = tempfile.mkdtemp(prefix=".awh-export-", dir=REPO_ROOT)
        self.addCleanup(shutil.rmtree, scratch, True)
        relative = os.path.join(os.path.basename(scratch), "exported-relative.dsk")

        async def go():
            async with mcp_session(self.errlog) as s:
                exported = await call(s, "export_disk", drive=1, path=dest)
                self.assertFalse(exported.is_error, text_of(exported))
                exported = await call(s, "export_disk", drive=1, path=relative)
                self.assertFalse(exported.is_error, text_of(exported))
                again = await call(s, "export_disk", drive=1, path=dest)   # the harness never overwrites
                self.assertTrue(again.is_error)
                self.assertIn("already exists", text_of(again))
                empty = await call(s, "export_disk", drive=2, path=os.path.join(FIXTURE_DIR, "empty.dsk"))
                self.assertTrue(empty.is_error)
                self.assertIn("empty", text_of(empty))
                bad_drive = await call(s, "export_disk", drive=3, path=os.path.join(FIXTURE_DIR, "x.dsk"))
                self.assertTrue(bad_drive.is_error)
                unc = await call(s, "export_disk", drive=1, path="\\\\server\\share\\x.dsk")
                self.assertTrue(unc.is_error)
                self.assertIn("not allowed", text_of(unc))   # rejected by the adapter, never sent to the harness
        self.run_async(go)
        self.assertEqual(sha256_file(dest), master_before)   # nothing was written to the disk yet
        self.assertEqual(sha256_file(os.path.join(REPO_ROOT, relative)), master_before)
        self.assertEqual(sha256_file(DOS_IMAGE), master_before)


class ShutdownTest(AdapterTestCase):
    def test_closing_the_client_ends_the_harness(self):
        """Review Focus 2: the harness process does not outlive the MCP connection."""
        pids = []

        async def go():
            async with mcp_session(self.errlog) as s:
                result = await call(s, "wait", ms=100)   # the harness is up and a session exists
                self.assertFalse(result.is_error, text_of(result))
                pids.append(self.harness_pid())
                self.assertTrue(pid_alive(pids[0]))
        self.run_async(go)
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline and pid_alive(pids[0]):
            time.sleep(0.2)
        if pid_alive(pids[0]):
            subprocess.call(["taskkill", "/F", "/PID", str(pids[0])], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
        self.assertFalse(pid_alive(pids[0]), "the harness outlived the closed MCP client by 10 s")


class ListenTest(AdapterTestCase):
    def test_bell_then_listen_gives_one_pair_then_nothing(self):
        """Review Focus 5: events are buffered from launch and drained by each listen."""
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=12000)   # boot DOS 3.3
                await call(s, "listen")   # discard the boot beep, buffered since launch
                typed = await call(s, "type_text", text="PRINT CHR$(7)\n")
                self.assertFalse(typed.is_error, text_of(typed))
                await call(s, "wait", ms=1000)
                first = await call(s, "listen")
                self.assertFalse(first.is_error, text_of(first))
                text = text_of(first)
                self.assertEqual(text.count("sound_started"), 1, text)
                self.assertEqual(text.count("sound_stopped"), 1, text)
                self.assertIn("dominant_hz", text)
                second = await call(s, "listen")
                self.assertFalse(second.is_error, text_of(second))
                self.assertNotIn("sound_started", text_of(second))
                self.assertNotIn("sound_stopped", text_of(second))
        self.run_async(go)


class LimitErrorTest(AdapterTestCase):
    def test_harness_limit_errors_come_back_as_tool_errors_with_the_harness_message(self):
        """M6: the adapter passes the harness limits through (hold_ms 0-10000, duration_ms 0-60000,
        inter_key_delay_ms 0-1000)."""
        async def go():
            async with mcp_session(self.errlog) as s:
                for name, args, field in (("press_key", {"key": "A", "hold_ms": 10001}, "hold_ms"),
                                          ("set_joystick", {"x": 1, "duration_ms": 60001}, "duration_ms"),
                                          ("type_text", {"text": "A", "inter_key_delay_ms": 1001}, "inter_key_delay_ms")):
                    with self.subTest(tool=name):
                        result = await call(s, name, **args)
                        self.assertTrue(result.is_error, "{} {} must be rejected".format(name, args))
                        self.assertIn(field, text_of(result))   # the harness's -32602 message, not an opaque failure
        self.run_async(go)


class AdapterLoggingTest(AdapterTestCase):
    SECRET = "QZXADAPTERSECRET"

    def test_json_log_lines_have_the_required_fields(self):
        async def go():
            async with mcp_session(self.errlog, extra_args=["--log-format", "json"]) as s:
                await call(s, "wait", ms=100)
                typed = await call(s, "type_text", text="10 REM " + self.SECRET + "\n")
                self.assertFalse(typed.is_error, text_of(typed))
                await call(s, "press_key", key="A", hold_ms=10001)   # rejected by the harness
        self.run_async(go)
        lines = [l for l in self.stderr_text().splitlines() if l.strip()]
        self.assertTrue(lines, "the adapter wrote nothing to stderr")
        records = []
        for line in lines:
            with self.subTest(line=line[:120]):
                record = json.loads(line)
                self.assertRegex(record["ts"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")
                self.assertIn(record["level"], ("debug", "info", "warning", "error"))
                self.assertIsInstance(record["msg"], str)
                self.assertIn("component", record)
                records.append(record)
        adapter = [r for r in records if r["component"] == "adapter"]
        self.assertTrue(adapter, "no record with component 'adapter'")
        self.assertTrue(any(re.search(r"harness pid \d+", r["msg"]) for r in adapter), "the 'harness pid N' line")
        self.assertNotIn(self.SECRET, self.stderr_text())

    def test_the_default_format_is_text_with_the_pid_line(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                await call(s, "wait", ms=100)
        self.run_async(go)
        self.assertRegex(self.stderr_text(), r"harness pid \d+")
        self.assertFalse(self.stderr_text().lstrip().startswith("{"))

    def test_an_invalid_log_format_is_a_usage_error(self):
        env = dict(os.environ, PYTHONPATH=ADAPTER_DIR)
        r = subprocess.run([sys.executable, "-m", "applewin_mcp", "--exe", exe_path(), "--log-format", "xml"], env=env,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertEqual(r.stdout, b"")


class SpeedOptionTest(AdapterTestCase):
    def parse(self, *argv):
        from applewin_mcp import server
        return server.parse_args(["--exe", "x.exe", *argv])

    def session_params(self, *argv):
        from applewin_mcp import server
        return server.session_params(self.parse(*argv))

    def test_no_speed_flags_leave_the_session_speed_alone(self):
        self.assertNotIn("speed", self.session_params("--realtime"))

    def test_speed_and_disk_boost_flags_go_into_create_session(self):
        self.assertEqual(self.session_params("--realtime", "--speed", "20")["speed"], {"machine_speed": 20})
        self.assertEqual(self.session_params("--realtime", "--speed", "max")["speed"], {"machine_speed": "max"})
        self.assertEqual(self.session_params("--realtime", "--no-disk-boost")["speed"], {"enhance_disk_speed": False})
        self.assertEqual(self.session_params("--realtime", "--speed", "authentic", "--no-disk-boost")["speed"],
                         {"machine_speed": "authentic", "enhance_disk_speed": False})

    def test_bad_speed_values_are_usage_errors(self):
        for bad in ("41", "-1", "fast", "1.5"):
            with self.subTest(speed=bad), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as ctx:
                    self.parse("--realtime", "--speed", bad)
                self.assertEqual(ctx.exception.code, 2)

    def test_speed_flags_need_realtime(self):
        for flags in (["--speed", "20"], ["--no-disk-boost"]):
            with self.subTest(flags=flags), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as ctx:
                    self.parse(*flags)
                self.assertEqual(ctx.exception.code, 2)

    def test_set_speed_changes_the_speed_while_running(self):
        async def go():
            async with mcp_session(self.errlog, extra_args=["--realtime"]) as s:
                current = await call(s, "set_speed")   # no arguments: reports the speed
                self.assertFalse(current.is_error, text_of(current))
                self.assertIn("machine_speed=10", text_of(current))
                faster = await call(s, "set_speed", speed="20", disk_boost=False)
                self.assertFalse(faster.is_error, text_of(faster))
                self.assertIn("machine_speed=20", text_of(faster))
                self.assertIn("enhance_disk_speed=False", text_of(faster))
                unthrottled = await call(s, "set_speed", speed="max")
                self.assertIn("machine_speed=40", text_of(unthrottled))
                self.assertIn("enhance_disk_speed=False", text_of(unthrottled))   # left out: unchanged
                for bad in ("41", "fast"):
                    with self.subTest(speed=bad):
                        result = await call(s, "set_speed", speed=bad)
                        self.assertTrue(result.is_error)
                        self.assertIn("0 to 40", text_of(result))
                waited = await call(s, "wait", ms=100)
                self.assertFalse(waited.is_error, text_of(waited))
        self.run_async(go)

    def test_set_speed_is_an_error_in_stepped_mode(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                result = await call(s, "set_speed", speed="20")
                self.assertTrue(result.is_error)
                self.assertIn("--realtime", text_of(result))
        self.run_async(go)

    def test_the_harness_accepts_the_speed_settings(self):
        async def go():
            async with mcp_session(self.errlog, extra_args=["--realtime", "--speed", "20", "--no-disk-boost"]) as s:
                waited = await call(s, "wait", ms=200)
                self.assertFalse(waited.is_error, text_of(waited))
        self.run_async(go)


class StderrWriterTest(unittest.TestCase):
    """A stderr that blocks must never stall a producer (the harness stderr forwarder, or a tool call that logs)."""

    def test_a_blocked_stderr_drops_lines_instead_of_blocking_and_reports_the_count(self):
        import threading
        from applewin_mcp.server import StderrWriter
        gate, got = threading.Event(), []

        def target(data):
            gate.wait(20)
            got.append(data)

        w = StderrWriter(maxsize=5, target=target)
        t0 = time.monotonic()
        for i in range(100):
            w.write("line {}\n".format(i).encode())
        self.assertLess(time.monotonic() - t0, 2.0, "write blocked on a blocked stderr")
        gate.set()
        w.close(timeout=5)
        self.assertFalse(w.thread.is_alive())
        text = b"".join(got).decode()
        self.assertRegex(text, r"\d+ log lines dropped")
        self.assertIn("line 0\n", text)
        self.assertTrue(all(l.startswith(("line ", "warning")) for l in text.splitlines()), text)

    def test_close_gives_up_when_stderr_stays_blocked(self):
        import threading
        from applewin_mcp.server import StderrWriter
        gate = threading.Event()
        w = StderrWriter(maxsize=1, target=lambda d: gate.wait(20))
        w.write(b"a\n")
        w.write(b"b\n")
        t0 = time.monotonic()
        w.close(timeout=0.3)
        self.assertLess(time.monotonic() - t0, 2.0)
        gate.set()


class VersionTest(unittest.TestCase):
    def test_the_adapter_version_is_the_version_file(self):
        import applewin_mcp
        with open(os.path.join(REPO_ROOT, "VERSION"), encoding="ascii") as f:
            self.assertEqual(applewin_mcp.__version__, f.read().strip())


# ---- Pass condition 7 / Review Focus 5: the extracted bundle works on its own ----

def bundle_root():
    d = os.path.abspath(os.environ["HARNESS_BUNDLE_DIR"])
    if not os.path.isfile(os.path.join(d, "AppleWinHarness.exe")):
        children = [os.path.join(d, c) for c in os.listdir(d) if os.path.isfile(os.path.join(d, c, "AppleWinHarness.exe"))]
        if len(children) == 1:
            return children[0]
    return d


@unittest.skipUnless(os.environ.get("HARNESS_BUNDLE_DIR"), "set HARNESS_BUNDLE_DIR to the extracted bundle")
class BundleSmokeTest(AdapterTestCase):
    """The M5 end-to-end flow with the bundle's exe and adapter package only (PYTHONPATH=<bundle>/adapter), from a
    working directory outside the repository. Needs `pip install -r adapter/requirements.txt` and the DOS master."""

    def setUp(self):
        super().setUp()
        self.bundle = bundle_root()
        self.cwd = tempfile.mkdtemp(prefix="awh-m6-bundle-cwd-")
        self.addCleanup(shutil.rmtree, self.cwd, True)
        env = dict(os.environ)
        for name in ("HARNESS_EXE", "PYTHONPATH"):
            env.pop(name, None)
        env["PYTHONPATH"] = os.path.join(self.bundle, "adapter")
        env["PYTHONDONTWRITEBYTECODE"] = "1"   # keep the extracted bundle exactly as the layout test expects
        self.env = env

    def params(self):
        return StdioServerParameters(
            command=sys.executable, env=self.env, cwd=self.cwd,
            args=["-m", "applewin_mcp", "--exe", os.path.join(self.bundle, "AppleWinHarness.exe"), "--headless",
                  "--disk", DOS_IMAGE, "--text-screen"])

    def test_the_adapter_package_comes_from_the_bundle(self):
        out = subprocess.run([sys.executable, "-c", "import applewin_mcp; print(applewin_mcp.__file__)"], env=self.env,
                             cwd=self.cwd, stdout=subprocess.PIPE, text=True, timeout=60).stdout.strip()
        self.assertTrue(os.path.normcase(os.path.realpath(out)).startswith(
            os.path.normcase(os.path.realpath(self.bundle))), out)

    def test_boot_type_run_read_and_look_from_the_bundle(self):
        async def go():
            async with stdio_client(self.params(), errlog=self.errlog) as (read, write):
                async with ClientSession(read, write) as session:
                    await asyncio.wait_for(session.initialize(), CALL_TIMEOUT)
                    await m5_end_to_end(self, session)
        self.run_async(go)


if __name__ == "__main__":
    unittest.main()
