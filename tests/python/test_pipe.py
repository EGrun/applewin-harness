"""AppleWinHarness.exe --serve over its named pipe (spec: docs/specs/m3.md, Part A; pass conditions 1-5).

Run: python -m unittest discover -s tests/python -v   (HARNESS_EXE selects the exe; default bin/x64/Debug).
Windowed tests are skipped unless HARNESS_WINDOWED=1.
"""

import base64
import hashlib
import json
import os
import struct
import threading
import time
import unittest
import zlib

from harness_client import REPO_ROOT, Harness, RpcError

CYCLES_PER_FRAME = 17030
BASE_CLOCK_HZ = (157500000.0 / 11.0) * 65.0 / 912.0   # CLK_6502_NTSC
TEST_TIMEOUT_S = 60.0
DOS_IMAGE = os.path.join(REPO_ROOT, "external", "AppleWin", "bin", "DOS 3.3 System Master - 680-0210-A.dsk")
WINDOWED = os.environ.get("HARNESS_WINDOWED") == "1"


def ms_to_cycles(ms):
    return int(ms * BASE_CLOCK_HZ / 1000.0)


def golden(name):
    with open(os.path.join(REPO_ROOT, "tests", "golden", name), "r", encoding="ascii") as f:
        return f.readline().strip()


def dos_disks():
    return [{"slot": 6, "drive": 1, "path": DOS_IMAGE}]


def png_rgb_sha256(png):
    """SHA-256 of the RGB pixels, top row first (harness::FrameHash), of an 8-bit RGB PNG with filter-0 rows."""
    assert png[:8] == b"\x89PNG\r\n\x1a\n", "bad PNG signature"
    pos, idat, width, height = 8, b"", 0, 0
    while pos + 12 <= len(png):
        (length,) = struct.unpack(">I", png[pos:pos + 4])
        kind = png[pos + 4:pos + 8]
        data = png[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", png[pos + 8 + length:pos + 12 + length])
        assert zlib.crc32(kind + data) & 0xFFFFFFFF == crc, "bad CRC in {!r}".format(kind)
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", data[:10])
            assert (depth, color) == (8, 2), "expected 8-bit RGB, got depth {} colour type {}".format(depth, color)
        elif kind == b"IDAT":
            idat += data
        elif kind == b"IEND":
            break
        pos += 12 + length
    raw = zlib.decompress(idat)
    row_bytes = width * 3
    assert len(raw) == (row_bytes + 1) * height, "IDAT has the wrong size"
    rgb = bytearray()
    for y in range(height):
        row = raw[y * (row_bytes + 1):(y + 1) * (row_bytes + 1)]
        assert row[0] == 0, "row {} uses filter {}".format(y, row[0])
        rgb += row[1:]
    return width, height, hashlib.sha256(bytes(rgb)).hexdigest()


def text_row_band(frame, row):
    """The BGRA32 pixels of 40-column text row `row` (16 pixel rows in the 560x384 frame)."""
    data = base64.b64decode(frame["data"])
    stride = frame["stride"]
    return data[row * 16 * stride:(row + 1) * 16 * stride]


class PipeTestCase(unittest.TestCase):
    def launch(self, headless=True):
        """A server for this test: closed on cleanup, killed by a watchdog after TEST_TIMEOUT_S."""
        harness = Harness.launch(headless=headless)
        self.addCleanup(harness.close)
        watchdog = threading.Timer(TEST_TIMEOUT_S, harness.kill)
        watchdog.daemon = True
        watchdog.start()
        self.addCleanup(watchdog.cancel)
        return harness

    def assertRpcError(self, code, conn, method, **params):
        with self.assertRaises(RpcError) as ctx:
            conn.call(method, timeout=30.0, **params)
        self.assertEqual(ctx.exception.code, code, "{} {}: {}".format(method, params, ctx.exception))

    def stepped_session(self, harness, disks=None):
        op = harness.connect("operator")
        params = {"run_mode": "stepped"}
        if disks is not None:
            params["disks"] = disks
        op.call("create_session", timeout=30.0, **params)
        return op


# ---- Pass condition 1: M2 over the pipe ----

class M2OverPipeTest(PipeTestCase):
    def run_m2_scenario(self, headless):
        h = self.launch(headless=headless)
        op = h.connect("operator")
        self.assertEqual(op.hello_reply, {"protocol_version": "0.3", "role": "operator",
                                          "display_mode": "headless" if headless else "windowed"})
        created = op.call("create_session", disks=dos_disks(), run_mode="stepped")
        self.assertIn("session_id", created)
        stepped = op.call("step_frames", frames=720)
        self.assertGreaterEqual(stepped["emulated_cycles"], 720 * CYCLES_PER_FRAME)
        self.assertLess(stepped["emulated_cycles"], 720 * CYCLES_PER_FRAME + 100)

        agent = h.connect("agent")
        typed = agent.call("type_text", text="PRINT 2+2\n", inter_key_delay_ms=0)
        self.assertIn("input_id", typed)
        self.assertIn("applied_at_cycle", typed)
        op.call("step_frames", frames=60)

        frame = agent.call("get_frame", format="PNG")
        self.assertEqual(frame["format"], "PNG")
        self.assertEqual((frame["width"], frame["height"]), (560, 384))
        width, height, digest = png_rgb_sha256(base64.b64decode(frame["data"]))
        self.assertEqual((width, height), (560, 384))
        self.assertEqual(digest, golden("m2_dos_print.sha256"))

    def test_m2_golden_over_pipe(self):
        self.run_m2_scenario(headless=True)

    @unittest.skipUnless(WINDOWED, "set HARNESS_WINDOWED=1 to run windowed tests")
    def test_m2_golden_over_pipe_windowed(self):
        self.run_m2_scenario(headless=False)


# ---- Launch ----

class LaunchTest(PipeTestCase):
    def test_announcement_line(self):
        h = self.launch()
        line = h.announcement_line.decode("ascii").rstrip("\r\n")
        self.assertRegex(line, r'^\{"pipe":"\\\\\\\\\.\\\\pipe\\\\applewin-harness-[0-9a-f]{32}",'
                               r'"agent_token":"[0-9a-f]{64}","operator_token":"[0-9a-f]{64}","pid":[1-9][0-9]*\}$')
        self.assertEqual(sorted(h.announcement), ["agent_token", "operator_token", "pid", "pipe"])
        self.assertEqual(h.pid, h.process.pid)
        self.assertNotEqual(h.agent_token, h.operator_token)


# ---- Pass condition 2: security (token and role) ----

class SecurityTest(PipeTestCase):
    def test_wrong_token_gets_32001_and_closes(self):
        h = self.launch()
        for role, token in (("operator", "0" * 64), ("operator", h.agent_token), ("agent", h.operator_token),
                            ("admin", h.operator_token)):
            with self.subTest(role=role, token=token[:8]):
                conn = h.open_raw()
                with self.assertRaises(RpcError) as ctx:
                    conn.call("hello", timeout=10.0, protocol_version="0.3", role=role, token=token)
                self.assertEqual(ctx.exception.code, -32001)
                self.assertTrue(conn.wait_closed(5.0), "connection not closed")

    def test_method_before_hello_gets_32001_and_closes(self):
        h = self.launch()
        conn = h.open_raw()
        with self.assertRaises(RpcError) as ctx:
            conn.call("get_frame", timeout=10.0)
        self.assertEqual(ctx.exception.code, -32001)
        self.assertTrue(conn.wait_closed(5.0), "connection not closed")

    def test_agent_calling_operator_method_gets_32001_and_stays_open(self):
        h = self.launch()
        agent = h.connect("agent")
        self.assertRpcError(-32001, agent, "step_frames", frames=1)
        self.assertRpcError(-32001, agent, "create_session", run_mode="stepped")
        self.assertFalse(agent.closed)
        self.assertRpcError(-32002, agent, "get_frame")   # still served: no session yet


# ---- Pass condition 3: framing ----

class FramingTest(PipeTestCase):
    def test_length_over_16_mib_closes_the_connection(self):
        h = self.launch()
        conn = h.connect("operator")
        conn.send_raw(struct.pack("<I", 16 * 1024 * 1024 + 1))
        self.assertTrue(conn.wait_closed(5.0), "connection not closed")
        h.connect("operator")   # the server still serves

    def test_malformed_json_gets_32700_and_stays_open(self):
        h = self.launch()
        conn = h.connect("operator")
        conn.send_body("{not json")
        reply = conn.wait_reply(None, timeout=10.0)
        self.assertEqual(reply["error"]["code"], -32700)
        time.sleep(0.3)
        self.assertFalse(conn.closed)
        self.assertRpcError(-32601, conn, "no_such_method")

    def test_half_frame_disconnect_leaves_the_session_running(self):
        """Review focus 2: a client that goes away mid-message closes only its own connection."""
        h = self.launch()
        op = self.stepped_session(h)
        agent = h.connect("agent")
        agent.send_raw(struct.pack("<I", 100) + b'{"jsonrpc"')
        agent.close()
        time.sleep(0.2)
        stepped = op.call("step_frames", timeout=30.0, frames=10)
        self.assertGreater(stepped["emulated_cycles"], 0)
        frame = h.connect("agent").call("get_frame", timeout=30.0, format="BGRA32")
        self.assertEqual(frame["format"], "BGRA32")


# ---- Pass condition 4: joystick ----

class JoystickTest(PipeTestCase):
    def test_pdl0_and_button0(self):
        h = self.launch()
        op = self.stepped_session(h, disks=dos_disks())
        op.call("step_frames", frames=720)
        agent = h.connect("agent")

        def run(line):
            # HOME clears the screen, so what the statement prints lands on text row 0, away from the cursor.
            agent.call("type_text", text="HOME:" + line + "\n", inter_key_delay_ms=0)
            op.call("step_frames", frames=60)
            return text_row_band(agent.call("get_frame", format="BGRA32"), 0)

        agent.call("set_joystick", x=0)
        pdl_low = run("PRINT PDL(0)")
        self.assertEqual(pdl_low, run("PRINT 0"), "PDL(0) did not print 0 at x=0")
        agent.call("set_joystick", x=255)
        pdl_high = run("PRINT PDL(0)")
        self.assertEqual(pdl_high, run("PRINT 255"), "PDL(0) did not print 255 at x=255")
        self.assertNotEqual(pdl_low, pdl_high)

        agent.call("set_joystick", buttons={"0": True})
        self.assertEqual(run("PRINT PEEK(-16287)>127"), run("PRINT 1"), "button 0 down not seen")
        agent.call("set_joystick", buttons={"0": False})
        self.assertEqual(run("PRINT PEEK(-16287)>127"), run("PRINT 0"), "button 0 up not seen")


# ---- Pass condition 5: execution control ----

class ExecutionControlTest(PipeTestCase):
    def test_pause_and_resume_in_a_realtime_session(self):
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", run_mode="realtime")
        agent = h.connect("agent")
        time.sleep(0.3)
        paused = op.call("pause")["emulated_cycles"]
        self.assertGreater(paused, 0)
        # Observe the emulator through get_frame (not the pause reply) before and after 500 ms of wall-clock time.
        before = agent.call("get_frame", format="BGRA32")
        self.assertEqual(before["emulated_cycles"], paused)
        time.sleep(0.5)
        after = agent.call("get_frame", format="BGRA32")
        self.assertEqual((after["frame_id"], after["emulated_cycles"]), (before["frame_id"], before["emulated_cycles"]),
                         "the emulator ran while paused")
        self.assertEqual(op.call("pause")["emulated_cycles"], paused, "pause while paused did not reply at once")
        self.assertEqual(op.call("resume"), {})
        time.sleep(0.5)
        resumed = agent.call("get_frame", format="BGRA32")
        self.assertGreater(resumed["emulated_cycles"], paused, "cycles did not advance after resume")
        self.assertGreater(resumed["frame_id"], before["frame_id"], "no frames presented after resume")

    def test_resume_in_a_stepped_session_gets_32008(self):
        h = self.launch()
        op = self.stepped_session(h)
        self.assertRpcError(-32008, op, "resume")

    def test_step_frames_limits(self):
        h = self.launch()
        op = self.stepped_session(h)
        self.assertRpcError(-32602, op, "step_frames", frames=0)
        self.assertRpcError(-32602, op, "step_frames", frames=100001)
        self.assertRpcError(-32602, op, "step_frames")

    def test_get_frame_during_a_long_step_is_answered_after_it(self):
        """Review focus 4: a command that arrives during step_frames is processed after the step, in order."""
        h = self.launch()
        op = self.stepped_session(h)
        agent = h.connect("agent")
        before = agent.call("get_frame", format="BGRA32")["frame_id"]
        step_id = op.request("step_frames", {"frames": 3000})
        time.sleep(0.3)
        frame_id = agent.request("get_frame", {"format": "BGRA32"})
        op.result_of(step_id, timeout=50.0)
        during = agent.result_of(frame_id, timeout=50.0)
        after = agent.call("get_frame", format="BGRA32")
        self.assertEqual(during["frame_id"], after["frame_id"])
        self.assertGreaterEqual(during["frame_id"], before + 2999)


# ---- Sessions ----

class SessionTest(PipeTestCase):
    def test_second_create_session_gets_32008(self):
        h = self.launch()
        op = self.stepped_session(h)
        self.assertRpcError(-32008, op, "create_session", run_mode="stepped")

    def test_agent_methods_before_a_session_get_32002(self):
        h = self.launch()
        agent = h.connect("agent")
        self.assertRpcError(-32002, agent, "get_frame")
        self.assertRpcError(-32002, agent, "subscribe_frames", every_n_frames=1)
        self.assertRpcError(-32002, agent, "type_text", text="A")
        self.assertRpcError(-32002, agent, "press_key", key="A")
        self.assertRpcError(-32002, agent, "set_joystick", x=1)

    def test_missing_disk_gets_32005(self):
        h = self.launch()
        op = h.connect("operator")
        missing = os.path.join(REPO_ROOT, "tests", "no-such-image.dsk")
        self.assertFalse(os.path.exists(missing))
        self.assertRpcError(-32005, op, "create_session", run_mode="stepped",
                            disks=[{"slot": 6, "drive": 1, "path": missing}])

    def test_relative_disk_path_gets_32602(self):
        h = self.launch()
        op = h.connect("operator")
        self.assertRpcError(-32602, op, "create_session", run_mode="stepped",
                            disks=[{"slot": 6, "drive": 1, "path": "dos33.dsk"}])

    def test_deferred_options_get_32006(self):
        cases = [{"allow_text_screen": True}, {"shared_memory_frames": True}, {"model": "apple2"},
                 {"speed": {"machine_speed": 10}}]
        for extra in cases:
            with self.subTest(**{k: json.dumps(v) for k, v in extra.items()}):
                h = self.launch()
                op = h.connect("operator")
                self.assertRpcError(-32006, op, "create_session", run_mode="stepped", **extra)


# ---- Agent methods ----

class AgentMethodsTest(PipeTestCase):
    def test_get_frame_bgra32_layout(self):
        h = self.launch()
        op = self.stepped_session(h)
        op.call("step_frames", frames=2)
        agent = h.connect("agent")
        for borders, size in ((False, (560, 384)), (True, (600, 420))):
            with self.subTest(borders=borders):
                f = agent.call("get_frame", format="BGRA32", borders=borders)
                self.assertEqual(f["format"], "BGRA32")
                self.assertEqual((f["width"], f["height"]), size)
                self.assertEqual(f["stride"], f["width"] * 4)
                self.assertEqual(len(base64.b64decode(f["data"])), f["stride"] * f["height"])
                for key in ("frame_id", "emulated_cycles"):
                    self.assertIn(key, f)
        self.assertEqual(agent.call("get_frame")["format"], "PNG")   # the default
        self.assertRpcError(-32602, agent, "get_frame", format="JPEG")

    def test_subscribe_frames_every_2_over_10_frames(self):
        h = self.launch()
        op = self.stepped_session(h)
        # The first present comes about one frame after power-on (AppleWin presents once the beam leaves the vertical
        # blank), so the first 10 frames hold only 9 presents. One frame first puts the presents in step.
        op.call("step_frames", frames=1)
        agent = h.connect("agent")
        agent.call("subscribe_frames", every_n_frames=2)
        op.call("step_frames", frames=10)
        frames = [n for n in agent.drain_notifications(quiet=0.5) if n.get("method") == "frame_ready"]
        self.assertEqual(len(frames), 5)
        ids = [n["params"]["frame_id"] for n in frames]
        self.assertEqual([b - a for a, b in zip(ids, ids[1:])], [2, 2, 2, 2])
        for n in frames:
            self.assertEqual(sorted(n["params"]), ["emulated_cycles", "frame_id", "height", "width"])
            self.assertEqual((n["params"]["width"], n["params"]["height"]), (560, 384))

        agent.call("subscribe_frames", every_n_frames=0)   # unsubscribe
        op.call("step_frames", frames=10)
        self.assertEqual([n for n in agent.drain_notifications(quiet=0.5) if n.get("method") == "frame_ready"], [])
        self.assertRpcError(-32602, agent, "subscribe_frames", every_n_frames=601)
        self.assertRpcError(-32602, agent, "subscribe_frames", every_n_frames=-1)

    def test_input_applied_arrives_unprompted(self):
        """Review focus 1: a notification reaches a client that is idle in a read."""
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", run_mode="realtime")
        agent = h.connect("agent")
        typed = agent.call("type_text", text="A", inter_key_delay_ms=0)
        note = agent.next_notification(timeout=10.0)
        self.assertEqual(note.get("method"), "input_applied")
        params = note["params"]
        self.assertEqual(params["input_id"], typed["input_id"])
        self.assertLessEqual(params["first_cycle"], params["last_cycle"])
        if typed["applied_at_cycle"] is not None:
            self.assertEqual(typed["applied_at_cycle"], params["first_cycle"])

    def test_applied_at_cycle_in_a_stepped_session(self):
        h = self.launch()
        op = self.stepped_session(h)
        now = op.call("step_frames", frames=10)["emulated_cycles"]
        agent = h.connect("agent")

        moved = agent.call("set_joystick", x=10)
        self.assertEqual(moved["applied_at_cycle"], now)   # nothing queued: takes effect at the next step (D10)
        with self.assertRaises(TimeoutError):
            agent.next_notification(timeout=0.5)            # stepped: nothing runs until a step
        now = op.call("step_frames", frames=1)["emulated_cycles"]
        note = agent.next_notification(timeout=5.0)
        self.assertEqual(note["method"], "input_applied")
        self.assertEqual(note["params"]["input_id"], moved["input_id"])
        self.assertEqual(note["params"]["first_cycle"], moved["applied_at_cycle"])
        self.assertEqual(note["params"]["last_cycle"], moved["applied_at_cycle"])

        pressed = agent.call("press_key", key="A", hold_ms=50)
        self.assertEqual(pressed["applied_at_cycle"], now)   # strobe clear: nothing has typed yet
        op.call("step_frames", frames=1)                       # ~17 ms: still held
        with self.assertRaises(TimeoutError):
            agent.next_notification(timeout=0.5)
        op.call("step_frames", frames=5)
        note = agent.next_notification(timeout=5.0)
        self.assertEqual(note["params"]["input_id"], pressed["input_id"])
        self.assertGreaterEqual(note["params"]["last_cycle"] - note["params"]["first_cycle"], ms_to_cycles(50))

        # No program reads the keyboard here, so "A" is still unread: the next key waits for the strobe.
        waiting = agent.call("type_text", text="B")
        self.assertIsNone(waiting["applied_at_cycle"])

    def test_input_parameters_are_validated(self):
        h = self.launch()
        self.stepped_session(h)
        agent = h.connect("agent")
        self.assertRpcError(-32602, agent, "type_text", text="x" * 4097)
        self.assertRpcError(-32602, agent, "type_text", text="a\tb")
        self.assertRpcError(-32602, agent, "type_text", text="café")
        self.assertRpcError(-32602, agent, "type_text")
        self.assertRpcError(-32602, agent, "press_key", key="FOO")
        self.assertRpcError(-32602, agent, "press_key", key="ab")
        self.assertRpcError(-32602, agent, "press_key", key=128)
        self.assertRpcError(-32602, agent, "set_joystick", x=256)
        self.assertRpcError(-32006, agent, "set_joystick", joystick=1, x=0)
        self.assertRpcError(-32006, agent, "set_joystick", buttons={"2": True})
        for key in ("RETURN", "ESC", "TAB", "DELETE", "LEFT", "RIGHT", "UP", "DOWN", "CTRL-A", "CTRL-Z", "RESET",
                    "a", " ", "~", 0, 127):
            with self.subTest(key=key):
                result = agent.call("press_key", key=key, hold_ms=0)
                self.assertIn("input_id", result)
        self.assertIn("input_id", agent.call("type_text", text="x" * 4095 + "\n"))


LAUNCHER = r'''
import os, subprocess, sys
exe = os.environ["HARNESS_EXE"]
p = subprocess.Popen([exe, "--serve", "--headless"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, cwd=os.path.dirname(exe))
sys.stdout.write(p.stdout.readline().decode("utf-8"))
sys.stdout.flush()
'''


class OrphanTest(unittest.TestCase):
    def test_server_exits_when_its_launcher_exits(self):
        import subprocess
        import sys
        import tempfile
        from harness_client import exe_path
        env = dict(os.environ, HARNESS_EXE=exe_path())
        out = subprocess.run([sys.executable, "-c", LAUNCHER], env=env, stdout=subprocess.PIPE, timeout=30).stdout
        announcement = json.loads(out.decode("utf-8"))
        pid = announcement["pid"]
        self.addCleanup(subprocess.call, ["taskkill", "/F", "/PID", str(pid)], stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)
        work = os.path.join(tempfile.gettempdir(), "applewin-harness", str(pid))
        ini = work + ".ini"
        deadline = time.time() + 10
        while time.time() < deadline and _pid_alive(pid):
            time.sleep(0.1)
        self.assertFalse(_pid_alive(pid), "the server outlived its launcher by 10 s")
        self.assertFalse(os.path.exists(work))
        self.assertFalse(os.path.exists(ini))


def _pid_alive(pid):
    import ctypes
    kernel32 = ctypes.windll.kernel32
    kernel32.OpenProcess.restype = ctypes.c_void_p
    handle = kernel32.OpenProcess(0x00100000 | 0x1000, False, pid)   # SYNCHRONIZE | QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    try:
        return kernel32.WaitForSingleObject(ctypes.c_void_p(handle), 0) == 0x102   # WAIT_TIMEOUT
    finally:
        kernel32.CloseHandle(ctypes.c_void_p(handle))


if __name__ == "__main__":
    unittest.main()
