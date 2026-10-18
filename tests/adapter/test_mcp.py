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
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest

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

ALL_TOOLS = ["eject_disk", "insert_disk", "listen", "look", "press_key", "read_screen", "set_joystick", "type_text",
             "wait"]
NO_TEXT_TOOLS = [t for t in ALL_TOOLS if t != "read_screen"]


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


@contextlib.asynccontextmanager
async def mcp_session(errlog, text_screen=True, extra_args=()):
    async with stdio_client(server_params(text_screen, extra_args), errlog=errlog) as (read, write):
        async with ClientSession(read, write) as session:
            await asyncio.wait_for(session.initialize(), CALL_TIMEOUT)
            yield session


async def call(session, name, **arguments):
    return await asyncio.wait_for(session.call_tool(name, arguments), CALL_TIMEOUT)


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
    def tool_names(self, text_screen):
        async def go():
            async with mcp_session(self.errlog, text_screen=text_screen) as s:
                return sorted(t.name for t in (await asyncio.wait_for(s.list_tools(), CALL_TIMEOUT)).tools)
        return self.run_async(go)

    def test_exactly_the_agent_tools_with_text_screen(self):
        self.assertEqual(self.tool_names(True), ALL_TOOLS)

    def test_read_screen_absent_without_text_screen(self):
        self.assertEqual(self.tool_names(False), NO_TEXT_TOOLS)

    def test_no_operator_method_is_reachable(self):
        async def go():
            async with mcp_session(self.errlog) as s:
                for name in ("step_frames", "checkpoint", "restore_checkpoint", "export_disk", "set_speed",
                             "get_state_checksum", "create_session", "close_session", "pause", "resume"):
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
                waited = await call(s, "wait", ms=12000)   # boot DOS 3.3
                self.assertFalse(waited.is_error, text_of(waited))
                for line in ('10 FOR I = 1 TO 10', '20 PRINT I', '30 NEXT I'):
                    typed = await call(s, "type_text", text=line + "\n")
                    self.assertFalse(typed.is_error, text_of(typed))
                shown = await call(s, "read_screen")
                self.assertFalse(shown.is_error, text_of(shown))
                self.assertIn("20 PRINT I", text_of(shown))   # visible at once, no wait in between
                ran = await call(s, "type_text", text="HOME:RUN\n")
                self.assertFalse(ran.is_error, text_of(ran))
                await call(s, "wait", ms=1500)
                screen = text_of(await call(s, "read_screen"))
                lines = screen.splitlines()
                self.assertTrue(lines[0].startswith("40col"), lines[:1])
                rows = [l.strip() for l in lines[1:]]
                self.assertEqual(rows[:10], [str(n) for n in range(1, 11)], screen)
                looked = await call(s, "look")
                self.assertFalse(looked.is_error, text_of(looked))
                images = [c for c in looked.content if c.type == "image"]
                self.assertEqual(len(images), 1)
                self.assertEqual(images[0].mime_type, "image/png")
                self.assertEqual(png_size(base64.b64decode(images[0].data)), (560, 384))
                # press_key takes a numeric ASCII code (13 = RETURN), not only a string
                schema = next(t for t in (await s.list_tools()).tools if t.name == "press_key").input_schema
                self.assertIn("integer", [o.get("type") for o in schema["properties"]["key"]["anyOf"]])
                pressed = await call(s, "press_key", key=13)
                self.assertFalse(pressed.is_error, text_of(pressed))
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


if __name__ == "__main__":
    unittest.main()
