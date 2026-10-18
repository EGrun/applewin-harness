"""AppleWinHarness.exe --serve over its named pipe (spec: docs/specs/m3.md, Parts A and B; pass conditions 1-5,
7 and 8).

Run: python -m unittest discover -s tests/python -v   (HARNESS_EXE selects the exe; default bin/x64/Debug).
Windowed tests are skipped unless HARNESS_WINDOWED=1.
"""

import array
import base64
import hashlib
import json
import math
import os
import struct
import threading
import time
import unittest
import zlib

import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "adapter"))   # applewin_mcp.harness_client

from applewin_mcp.harness_client import REPO_ROOT, Harness, RpcError

CYCLES_PER_FRAME = 17030
BASE_CLOCK_HZ = (157500000.0 / 11.0) * 65.0 / 912.0   # CLK_6502_NTSC
TEST_TIMEOUT_S = 60.0
DOS_IMAGE = os.path.join(REPO_ROOT, "external", "AppleWin", "bin", "DOS 3.3 System Master - 680-0210-A.dsk")
WINDOWED = os.environ.get("HARNESS_WINDOWED") == "1"
# Realtime rates measured over the pipe: +/-10% (the C++ realtime suite holds the spec's +/-2%, +/-3% on CI).
RATE_TOLERANCE = 0.10


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
        with open(os.path.join(REPO_ROOT, "VERSION"), encoding="ascii") as f:
            version = f.read().strip()
        self.assertEqual(op.hello_reply, {"protocol_version": "0.3", "role": "operator", "server_version": version,
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
        cases = [{"shared_memory_frames": True}, {"model": "apple2"}]
        for extra in cases:
            with self.subTest(**{k: json.dumps(v) for k, v in extra.items()}):
                h = self.launch()
                op = h.connect("operator")
                self.assertRpcError(-32006, op, "create_session", run_mode="stepped", **extra)


# ---- Speed (D11, M3 Part C) ----

SPEED_KEYS = ["base_clock_hz", "clock_multiplier", "effective_clock_hz", "enhance_disk_speed", "full_speed_active",
              "full_speed_override", "full_speed_reasons", "machine_speed", "video_refresh"]


class SpeedTest(PipeTestCase):
    def assertSpeed(self, got, machine_speed, **expected):
        """Checks a get_speed-shaped reply: exact key set, the machine speed, and any further fields given."""
        self.assertEqual(sorted(got), SPEED_KEYS)
        self.assertEqual(got["machine_speed"], machine_speed)
        self.assertEqual(got["video_refresh"], 60)
        self.assertAlmostEqual(got["base_clock_hz"], BASE_CLOCK_HZ, delta=1e-3)
        for key, value in expected.items():
            self.assertEqual(got[key], value, key)

    def scenario_hash(self, speed=None, set_speed=None):
        """The M2 scenario in a stepped session with the given speed settings; the SHA-256 of the final frame."""
        h = self.launch()
        op = h.connect("operator")
        params = {"run_mode": "stepped", "disks": dos_disks()}
        if speed is not None:
            params["speed"] = speed
        op.call("create_session", timeout=30.0, **params)
        if set_speed is not None:
            op.call("set_speed", **set_speed)
        op.call("step_frames", frames=720)
        agent = h.connect("agent")
        agent.call("type_text", text="PRINT 2+2\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=60)
        frame = agent.call("get_frame", format="PNG")
        return png_rgb_sha256(base64.b64decode(frame["data"]))[2]

    def boot_hashes(self, set_speed):
        """A stepped DOS boot with the given speed settings: the frame hash every 10 frames up to 720 (ruling C7)."""
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", timeout=30.0, run_mode="stepped", disks=dos_disks())
        if set_speed:
            op.call("set_speed", **set_speed)
        agent = h.connect("agent")
        hashes = []
        for _ in range(72):
            op.call("step_frames", frames=10)
            frame = agent.call("get_frame", format="PNG")
            hashes.append(png_rgb_sha256(base64.b64decode(frame["data"]))[2])
        self.assertEqual(op.call("get_speed")["enhance_disk_speed"], set_speed.get("enhance_disk_speed", True))
        return hashes

    def frames_per_wall_second(self, agent, wall_s):
        """Emulated cycles per wall second over `wall_s`, from the frame_ready notifications' arrival times."""
        agent.call("subscribe_frames", every_n_frames=1)
        first = last = None
        start = time.monotonic()
        while time.monotonic() - start < wall_s:
            try:
                note = agent.next_notification(timeout=0.2)
            except TimeoutError:
                continue
            if note.get("method") != "frame_ready":
                continue
            point = (time.monotonic(), note["params"]["emulated_cycles"])
            first = first or point
            last = point
        self.assertIsNotNone(first, "no frame_ready arrived")
        return (last[1] - first[1]) / (last[0] - first[0])

    def test_defaults(self):
        h = self.launch()
        op = self.stepped_session(h)
        got = op.call("get_speed")
        self.assertSpeed(got, 10, enhance_disk_speed=True, full_speed_override=False, full_speed_active=False,
                         full_speed_reasons=[])
        self.assertAlmostEqual(got["clock_multiplier"], 1.0, places=6)
        self.assertAlmostEqual(got["effective_clock_hz"], BASE_CLOCK_HZ, delta=1e-3)

    def test_set_speed_round_trips_and_replies_like_get_speed(self):
        h = self.launch()
        op = self.stepped_session(h)
        for value, speed, factor in ((5, 5, 0.75), ("authentic", 10, 1.0), (20, 20, 2.0), (0, 0, 0.5), (39, 39, 3.9)):
            with self.subTest(machine_speed=value):
                set_reply = op.call("set_speed", machine_speed=value)
                self.assertSpeed(set_reply, speed, full_speed_reasons=[])
                self.assertAlmostEqual(set_reply["effective_clock_hz"], BASE_CLOCK_HZ * factor, delta=1e-3)
                self.assertAlmostEqual(set_reply["clock_multiplier"], factor, places=6)
                self.assertEqual(op.call("get_speed"), set_reply)

    def test_max_has_a_null_clock_and_a_reason(self):
        h = self.launch()
        op = self.stepped_session(h)
        got = op.call("set_speed", machine_speed="max")
        self.assertSpeed(got, 40, effective_clock_hz=None, full_speed_reasons=["machine_speed_max"])
        self.assertEqual(op.call("get_speed"), got)
        self.assertEqual(op.call("set_speed", machine_speed=40), got)

    def test_clock_multiplier_converts_to_a_machine_speed(self):
        h = self.launch()
        op = self.stepped_session(h)
        # AppleWin's UseClockMultiplier: below 0.5 is raised to 0.5, below 1 is (x-0.5)*20, else x*10, at most 39.
        for x, speed in ((0.3, 0), (0.5, 0), (0.75, 5), (1.0, 10), (1.55, 15), (2.0, 20), (3.95, 39), (10, 39)):
            with self.subTest(clock_multiplier=x):
                self.assertSpeed(op.call("set_speed", clock_multiplier=x), speed)

    def test_fields_left_out_keep_their_values(self):
        h = self.launch()
        op = self.stepped_session(h)
        op.call("set_speed", enhance_disk_speed=False, full_speed_override=True)
        got = op.call("set_speed", machine_speed=7)
        self.assertSpeed(got, 7, enhance_disk_speed=False, full_speed_override=True,
                         full_speed_reasons=["override"])
        got = op.call("set_speed", full_speed_override=False)
        self.assertSpeed(got, 7, enhance_disk_speed=False, full_speed_override=False, full_speed_reasons=[])
        self.assertSpeed(op.call("set_speed"), 7, enhance_disk_speed=False)   # no fields: unchanged
        got = op.call("set_speed", machine_speed="max", full_speed_override=True)
        self.assertEqual(got["full_speed_reasons"], ["machine_speed_max", "override"])

    def test_invalid_params_get_32602_and_change_nothing(self):
        h = self.launch()
        op = self.stepped_session(h)
        before = op.call("set_speed", machine_speed=15)
        bad = [{"machine_speed": 5, "clock_multiplier": 1.0}, {"machine_speed": 41}, {"machine_speed": -1},
               {"machine_speed": "fast"}, {"machine_speed": 2.5}, {"machine_speed": True},
               {"clock_multiplier": 0}, {"clock_multiplier": -1}, {"clock_multiplier": "2"},
               {"enhance_disk_speed": "yes"}, {"full_speed_override": 1},
               {"video_refresh": 50}, {"machine_speed": 5, "video_refresh": 50}]
        for params in bad:
            with self.subTest(**{k: json.dumps(v) for k, v in params.items()}):
                self.assertRpcError(-32602, op, "set_speed", **params)
        self.assertEqual(op.call("get_speed"), before)

    def test_speed_methods_before_a_session_get_32002(self):
        h = self.launch()
        op = h.connect("operator")
        self.assertRpcError(-32002, op, "get_speed")
        self.assertRpcError(-32002, op, "set_speed", machine_speed=5)

    def test_speed_is_an_operator_method(self):
        h = self.launch()
        self.stepped_session(h)
        agent = h.connect("agent")
        self.assertRpcError(-32001, agent, "get_speed")
        self.assertRpcError(-32001, agent, "set_speed", machine_speed=5)
        self.assertEqual(agent.call("get_frame", format="BGRA32")["format"], "BGRA32")   # still open

    def test_create_session_speed_applies_its_settings(self):
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", run_mode="stepped",
                speed={"machine_speed": 20, "enhance_disk_speed": False, "full_speed_override": True,
                       "video_refresh": 60})
        got = op.call("get_speed")
        self.assertSpeed(got, 20, enhance_disk_speed=False, full_speed_override=True, full_speed_reasons=["override"])
        self.assertAlmostEqual(got["effective_clock_hz"], BASE_CLOCK_HZ * 2.0, delta=1e-3)

    def test_create_session_speed_accepts_clock_multiplier_and_max(self):
        for speed, expected in (({"clock_multiplier": 0.75}, 5), ({"machine_speed": "max"}, 40),
                                ({"machine_speed": "authentic"}, 10)):
            with self.subTest(speed=json.dumps(speed)):
                h = self.launch()
                op = h.connect("operator")
                op.call("create_session", run_mode="stepped", speed=speed)
                self.assertSpeed(op.call("get_speed"), expected)

    def test_create_session_speed_errors(self):
        cases = [(-32006, {"video_refresh": 50}), (-32006, {"machine_speed": 10, "video_refresh": 50}),
                 (-32602, {"machine_speed": 5, "clock_multiplier": 1.0}), (-32602, {"machine_speed": 41}),
                 (-32602, {"clock_multiplier": 0}), (-32602, {"enhance_disk_speed": "yes"}),
                 (-32602, {"video_refresh": 30}), (-32602, "fast")]
        for code, speed in cases:
            with self.subTest(code=code, speed=json.dumps(speed)):
                h = self.launch()
                op = h.connect("operator")
                self.assertRpcError(code, op, "create_session", run_mode="stepped", speed=speed)
                # The failed create left no session behind.
                self.assertRpcError(-32002, op, "get_speed")

    def test_stepped_hash_is_the_same_at_any_speed(self):
        """Review focus 1: stepped results do not depend on the speed settings (the M2 golden at every setting)."""
        expected = golden("m2_dos_print.sha256")
        runs = {
            "machine_speed 5": self.scenario_hash(set_speed={"machine_speed": 5}),
            "machine_speed 40": self.scenario_hash(set_speed={"machine_speed": 40}),
            "full_speed_override": self.scenario_hash(set_speed={"full_speed_override": True}),
            "create_session speed 40": self.scenario_hash(speed={"machine_speed": "max"}),
        }
        for name, digest in runs.items():
            with self.subTest(name):
                self.assertEqual(digest, expected)

    def test_stepped_boot_frames_are_the_same_at_any_speed_setting(self):
        """Ruling C7: stepped runs use AppleWin's defaults (speed 10, enhanced disk access), so the frames all through a
        DOS boot are the same with enhance_disk_speed off (AppleWin's disk timing differs mid-boot when it is off in
        a run) and at other speeds."""
        reference = self.boot_hashes({})
        for set_speed in ({"enhance_disk_speed": False}, {"machine_speed": 20, "enhance_disk_speed": False},
                          {"machine_speed": 5}):
            with self.subTest(json.dumps(set_speed)):
                hashes = self.boot_hashes(set_speed)
                differing = [10 * (i + 1) for i, (a, b) in enumerate(zip(hashes, reference)) if a != b]
                self.assertEqual(differing, [], "frames that differ")

    def test_set_speed_while_running_changes_the_pace(self):
        h = self.launch()
        op = h.connect("operator")
        # Enhance off: with no disk the boot ROM keeps the motor on, which would otherwise run at full speed.
        op.call("create_session", run_mode="realtime", speed={"enhance_disk_speed": False})
        agent = h.connect("agent")
        got = op.call("set_speed", machine_speed=20)
        self.assertEqual(got["full_speed_reasons"], [])
        time.sleep(0.5)
        rate = self.frames_per_wall_second(agent, 2.0)
        self.assertGreaterEqual(rate, 2.0 * BASE_CLOCK_HZ * (1 - RATE_TOLERANCE), "rate {:.0f}".format(rate))
        self.assertLessEqual(rate, 2.0 * BASE_CLOCK_HZ * (1 + RATE_TOLERANCE), "rate {:.0f}".format(rate))

    def test_frame_ready_is_capped_at_60_per_second_at_max(self):
        """Realtime at max: subscribe to every frame for 2 s of wall time; at most 60 per wall second arrive."""
        h = self.launch()
        op = h.connect("operator")
        # Enhance off, so max is the only reason (no disk_motor from the empty drive).
        op.call("create_session", run_mode="realtime", speed={"machine_speed": "max", "enhance_disk_speed": False})
        agent = h.connect("agent")
        agent.call("subscribe_frames", every_n_frames=1)
        first_cycles = agent.call("get_frame", format="BGRA32")["emulated_cycles"]
        start = time.monotonic()
        count = 0
        while time.monotonic() - start < 2.0:
            try:
                note = agent.next_notification(timeout=0.2)
            except TimeoutError:
                continue
            if note.get("method") == "frame_ready":
                count += 1
        elapsed = time.monotonic() - start
        last_cycles = agent.call("get_frame", format="BGRA32")["emulated_cycles"]
        # Cap: at most one frame_ready per 16 ms of wall time (ruling C4, the full-speed redraw interval) = 62.5/s,
        # plus 3 for the window edges.
        self.assertLessEqual(count, 62.5 * elapsed + 3, "{} frame_ready in {:.2f} s".format(count, elapsed))
        # Full speed really ran: far more than 2x authentic emulated cycles per wall second.
        rate = (last_cycles - first_cycles) / elapsed
        self.assertGreater(rate, 2 * BASE_CLOCK_HZ, "max ran at {:.0f} cycles/s".format(rate))
        self.assertGreater(count, 20, "frame_ready stopped arriving at max")
        speed = op.call("get_speed")
        self.assertEqual(speed["full_speed_reasons"], ["machine_speed_max"])
        self.assertTrue(speed["full_speed_active"])


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
        # (a 4,096-character text is covered in test_limits; after the 16 keys above it would pass the queue cap)
        self.assertIn("input_id", agent.call("type_text", text="x" * 4000 + "\n"))


# ---- Part B: agent audio (pass conditions 7 and 8; Review Focus 3 and 4) ----

# The //e bell's pitch as captured from PRINT CHR$(7) (measured in M3B Task 4: 934.5 Hz, 120 ms, peak -0.15 dBFS).
BELL_HZ = 935.0
SOUND_EVENTS = ("sound_started", "sound_stopped")


def parse_wav(wav):
    """(sample_rate, channels, bits, data bytes) of a canonical 44-byte-header PCM WAV."""
    assert len(wav) >= 44, "WAV shorter than its header"
    riff, riff_size, wave = struct.unpack("<4sI4s", wav[:12])
    fmt, fmt_size, audio_format, channels, rate, byte_rate, align, bits = struct.unpack("<4sIHHIIHH", wav[12:36])
    data_tag, data_size = struct.unpack("<4sI", wav[36:44])
    assert (riff, wave, fmt, data_tag) == (b"RIFF", b"WAVE", b"fmt ", b"data"), "not a canonical WAV header"
    assert (fmt_size, audio_format) == (16, 1), "not 16-byte PCM fmt"
    assert riff_size == 36 + data_size and len(wav) == 44 + data_size, "WAV sizes are inconsistent"
    assert byte_rate == rate * channels * bits // 8 and align == channels * bits // 8, "bad byte rate or block align"
    return rate, channels, bits, wav[44:]


def mono_ac_rms_db(pcm):
    """AC RMS (mean subtracted) in dBFS of the mono mix (L + R) / 2 of interleaved stereo int16 bytes."""
    samples = array.array("h")
    samples.frombytes(pcm)
    mono = [(samples[i] + samples[i + 1]) / 2.0 for i in range(0, len(samples) - 1, 2)]
    if not mono:
        return float("-inf")
    mean = sum(mono) / len(mono)
    rms = math.sqrt(sum((v - mean) ** 2 for v in mono) / len(mono))
    return 20.0 * math.log10(rms / 32768.0) if rms > 0 else float("-inf")


def frames_for(start_cycle, end_cycle):
    return (end_cycle - start_cycle) * 44100.0 / BASE_CLOCK_HZ


class AudioTest(PipeTestCase):
    def booted(self, harness):
        """A stepped session that has booted DOS 3.3 (720 frames), and an agent connection."""
        op = self.stepped_session(harness, disks=dos_disks())
        op.call("step_frames", frames=720, timeout=60.0)
        return op, harness.connect("agent")

    def sound_events(self, agent):
        return [n for n in agent.drain_notifications(quiet=0.5) if n.get("method") in SOUND_EVENTS]

    def test_bell_headless(self):
        """Pass condition 7."""
        h = self.launch()
        op, agent = self.booted(h)
        now = op.call("step_frames", frames=1)["emulated_cycles"]
        self.assertEqual(agent.call("subscribe_audio"), {})
        agent.call("type_text", text="PRINT CHR$(7)\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=120)

        events = self.sound_events(agent)
        self.assertEqual([e["method"] for e in events], ["sound_started", "sound_stopped"], events)
        started, stopped = events[0]["params"], events[1]["params"]
        self.assertEqual(sorted(started), ["emulated_ms", "start_cycle"])
        self.assertGreaterEqual(started["start_cycle"], now)
        self.assertAlmostEqual(started["emulated_ms"], started["start_cycle"] * 1000.0 / BASE_CLOCK_HZ, delta=1.0)
        self.assertEqual(sorted(stopped), ["dominant_hz", "duration_ms", "end_cycle", "peak_db", "start_cycle"])
        self.assertEqual(stopped["start_cycle"], started["start_cycle"])
        self.assertGreater(stopped["end_cycle"], stopped["start_cycle"])
        self.assertAlmostEqual(stopped["duration_ms"],
                               (stopped["end_cycle"] - stopped["start_cycle"]) * 1000.0 / BASE_CLOCK_HZ, delta=1.0)
        self.assertGreaterEqual(stopped["duration_ms"], 50)
        self.assertLessEqual(stopped["duration_ms"], 200)
        self.assertAlmostEqual(stopped["dominant_hz"], BELL_HZ, delta=0.1 * BELL_HZ)
        self.assertGreater(stopped["peak_db"], -50.0)

        audio = agent.call("get_audio", from_cycle=started["start_cycle"], duration_ms=200)
        self.assertEqual(audio["start_cycle"], started["start_cycle"])
        self.assertEqual((audio["sample_rate"], audio["channels"], audio["format"]), (44100, 2, "WAV"))
        self.assertAlmostEqual(audio["end_cycle"] - audio["start_cycle"], ms_to_cycles(200), delta=ms_to_cycles(1))
        rate, channels, bits, pcm = parse_wav(base64.b64decode(audio["data"]))
        self.assertEqual((rate, channels, bits), (44100, 2, 16))
        self.assertAlmostEqual(len(pcm) // 4, frames_for(audio["start_cycle"], audio["end_cycle"]), delta=1.0)
        self.assertGreater(mono_ac_rms_db(pcm), -30.0)

    def test_audio_older_than_the_ring_gets_32009(self):
        """Pass condition 7 (-32009) and Review Focus 4: the ring holds exactly the newest 30 s."""
        h = self.launch()
        op, agent = self.booted(h)
        op.call("step_frames", frames=1200, timeout=60.0)   # ~32 s of emulated time in all
        whole = agent.call("get_audio", duration_ms=30000, format="PCM16")   # the most recent 30 s: the whole ring
        self.assertAlmostEqual(whole["end_cycle"] - whole["start_cycle"], ms_to_cycles(30000), delta=ms_to_cycles(1))
        self.assertAlmostEqual(len(base64.b64decode(whole["data"])) // 4,
                               frames_for(whole["start_cycle"], whole["end_cycle"]), delta=1.0)
        self.assertRpcError(-32009, agent, "get_audio", from_cycle=0, duration_ms=10)
        just_older = whole["end_cycle"] - ms_to_cycles(30000) - ms_to_cycles(10)
        self.assertRpcError(-32009, agent, "get_audio", from_cycle=just_older, duration_ms=100)
        oldest = agent.call("get_audio", from_cycle=whole["start_cycle"], duration_ms=100, format="PCM16")
        self.assertEqual(oldest["start_cycle"], whole["start_cycle"])

    def test_get_audio_edges(self):
        """Review Focus 4, plus defaults, formats and parameter checks."""
        h = self.launch()
        agent0 = h.connect("agent")
        self.assertRpcError(-32002, agent0, "get_audio")
        self.assertRpcError(-32002, agent0, "subscribe_audio")
        op = self.stepped_session(h)
        now = op.call("step_frames", frames=120)["emulated_cycles"]
        agent = h.connect("agent")

        # A range past the newest sample is cut short.
        start = now - ms_to_cycles(100)
        cut = agent.call("get_audio", from_cycle=start, duration_ms=1000, format="PCM16")
        self.assertEqual(cut["start_cycle"], start)
        self.assertLessEqual(cut["end_cycle"], now)
        self.assertGreater(cut["end_cycle"], now - ms_to_cycles(50))
        self.assertEqual((cut["sample_rate"], cut["channels"], cut["format"]), (44100, 2, "PCM16"))
        self.assertAlmostEqual(len(base64.b64decode(cut["data"])) // 4, frames_for(cut["start_cycle"], cut["end_cycle"]),
                               delta=1.0)

        # Defaults: the most recent 1000 ms, as WAV.
        recent = agent.call("get_audio")
        self.assertEqual(recent["format"], "WAV")
        self.assertGreater(recent["end_cycle"], now - ms_to_cycles(50))
        self.assertLessEqual(recent["end_cycle"], now)
        self.assertAlmostEqual(recent["end_cycle"] - recent["start_cycle"], ms_to_cycles(1000), delta=ms_to_cycles(1))
        rate, channels, bits, pcm = parse_wav(base64.b64decode(recent["data"]))
        self.assertEqual((rate, channels, bits), (44100, 2, 16))
        self.assertAlmostEqual(len(pcm) // 4, frames_for(recent["start_cycle"], recent["end_cycle"]), delta=1.0)

        # WAV and PCM16 carry the same samples.
        at = now - ms_to_cycles(1500)
        wav = agent.call("get_audio", from_cycle=at, duration_ms=500, format="WAV")
        raw = agent.call("get_audio", from_cycle=at, duration_ms=500, format="PCM16")
        self.assertEqual((wav["start_cycle"], wav["end_cycle"]), (raw["start_cycle"], raw["end_cycle"]))
        self.assertEqual(parse_wav(base64.b64decode(wav["data"]))[3], base64.b64decode(raw["data"]))

        for bad in ({"duration_ms": 30001}, {"duration_ms": 0}, {"duration_ms": -1}, {"duration_ms": "1000"},
                    {"format": "MP3"}, {"from_cycle": -1}):
            with self.subTest(**{k: json.dumps(v) for k, v in bad.items()}):
                self.assertRpcError(-32602, agent, "get_audio", **bad)
        self.assertEqual(agent.call("get_audio", duration_ms=30000, format="PCM16")["format"], "PCM16")   # the limit

        self.assertEqual(agent.call("subscribe_audio", threshold_db=-40.5, min_silence_ms=50), {})
        self.assertRpcError(-32602, agent, "subscribe_audio", threshold_db="loud")
        self.assertRpcError(-32602, agent, "subscribe_audio", min_silence_ms=-1)

    def test_idle_dos_gives_no_sound_events(self):
        """Review Focus 3: the speaker's idle DC level is not sound."""
        h = self.launch()
        op, agent = self.booted(h)
        self.assertEqual(agent.call("subscribe_audio"), {})
        op.call("step_frames", frames=300)   # 5 s
        self.assertEqual(self.sound_events(agent), [])

    def capture_bell(self, headless):
        """The pass condition 8 script: boot DOS, PRINT CHR$(7), 120 frames; get_audio over a fixed cycle range."""
        h = self.launch(headless=headless)
        op, agent = self.booted(h)
        agent.call("type_text", text="PRINT CHR$(7)\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=120)
        audio = agent.call("get_audio", from_cycle=720 * CYCLES_PER_FRAME, duration_ms=1900, format="PCM16")
        h.close()
        return audio

    def assertSameAudio(self, a, b):
        self.assertEqual((a["start_cycle"], a["end_cycle"]), (b["start_cycle"], b["end_cycle"]))
        pcm = base64.b64decode(a["data"])
        self.assertGreater(mono_ac_rms_db(pcm), -40.0)   # the bell is in the range: not trivially equal silence
        self.assertTrue(pcm == base64.b64decode(b["data"]), "captured audio differs between the runs")

    def test_deterministic_audio(self):
        """Pass condition 8 (headless): two processes, byte-identical audio over the same cycles."""
        self.assertSameAudio(self.capture_bell(headless=True), self.capture_bell(headless=True))

    @unittest.skipUnless(WINDOWED, "set HARNESS_WINDOWED=1 to run windowed tests")
    def test_deterministic_audio_windowed(self):
        """Pass condition 8: headless and windowed give the same audio."""
        self.assertSameAudio(self.capture_bell(headless=True), self.capture_bell(headless=False))


LAUNCHER = r'''
import os, subprocess, sys
exe = os.environ["HARNESS_EXE"]
p = subprocess.Popen([exe, "--serve", "--headless"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, cwd=os.path.dirname(exe))
sys.stdout.write(p.stdout.readline().decode("utf-8"))
sys.stdout.flush()
'''


# ---- M5 pass condition 2: get_text_screen (docs/specs/m5.md) ----

class TextScreenTest(PipeTestCase):
    def test_without_allow_text_screen_gets_32006(self):
        h = self.launch()
        self.stepped_session(h)
        agent = h.connect("agent")
        self.assertRpcError(-32006, agent, "get_text_screen")

    def test_explicit_false_gets_32006(self):
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", run_mode="stepped", allow_text_screen=False)
        self.assertRpcError(-32006, h.connect("agent"), "get_text_screen")

    def test_40col_after_dos_boot_shows_prompt_row(self):
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", timeout=30.0, run_mode="stepped", allow_text_screen=True, disks=dos_disks())
        op.call("step_frames", frames=720)
        agent = h.connect("agent")
        agent.call("type_text", text='HOME:PRINT "HELLO"\n', inter_key_delay_ms=0)
        op.call("step_frames", frames=120)
        screen = agent.call("get_text_screen")
        self.assertEqual(sorted(screen), ["emulated_cycles", "lines", "mode"])
        self.assertEqual(screen["mode"], "40col")
        self.assertEqual(len(screen["lines"]), 24)
        self.assertTrue(all(len(line) == 40 for line in screen["lines"]), screen["lines"])
        self.assertEqual(screen["lines"][0], "HELLO".ljust(40))
        # Applesoft leaves a blank line between the PRINT output and the next prompt.
        self.assertEqual(screen["lines"][1], " " * 40)
        self.assertEqual(screen["lines"][2], "]".ljust(40))
        self.assertEqual(screen["emulated_cycles"], op.call("get_state_checksum")["emulated_cycles"])
        for line in screen["lines"]:
            self.assertTrue(all(0x20 <= ord(c) <= 0x7E for c in line), line)


class OrphanTest(unittest.TestCase):
    def test_server_exits_when_its_launcher_exits(self):
        import subprocess
        import sys
        import tempfile
        from applewin_mcp.harness_client import exe_path
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
