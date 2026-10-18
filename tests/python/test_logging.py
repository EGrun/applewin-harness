# SPDX-License-Identifier: MIT
"""Structured logging, `log` notifications, `disk_written`, the D10 short-path fix and the hello version (spec:
docs/specs/m6.md pass conditions 4, 5 and 6; plan Review Focus 2).

Run: python -m unittest discover -s tests/python -v   (HARNESS_EXE selects the exe; default bin/x64/Debug).
"""

import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "adapter"))   # applewin_mcp.harness_client

from applewin_mcp.harness_client import REPO_ROOT, Harness, exe_path
from test_pipe import DOS_IMAGE, PipeTestCase

BOOT_FRAMES = 720
TYPE_FRAMES = 240
TYPED_SECRET = "QZXTYPEDSECRET"   # a distinctive string no log line may contain
LEVELS = {"debug", "info", "warning", "error"}
COMPONENTS = {"main", "server", "session", "emulator", "audio"}
TS_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")


def read_version():
    with open(os.path.join(REPO_ROOT, "VERSION"), encoding="ascii") as f:
        return f.read().strip()


class LoggingTestCase(PipeTestCase):
    def launch_with(self, extra_args=(), env=None):
        harness = Harness.launch(headless=True, extra_args=extra_args, env=env)
        self.addCleanup(harness.close)
        watchdog = threading.Timer(120.0, harness.kill)
        watchdog.daemon = True
        watchdog.start()
        self.addCleanup(watchdog.cancel)
        return harness

    def booted(self, harness):
        op = harness.connect("operator")
        op.call("create_session", run_mode="stepped", allow_text_screen=True,
                disks=[{"slot": 6, "drive": 1, "path": DOS_IMAGE}])
        agent = harness.connect("agent")
        op.call("step_frames", frames=BOOT_FRAMES, timeout=60.0)
        return op, agent

    def finish(self, harness):
        """Closes the session, waits for the process to exit and returns (stdout after the announcement, stderr)."""
        conn = harness.connect("operator")
        conn.request("close_session", {})
        self.assertIsNotNone(harness.wait_exit(15.0), "the harness did not exit after close_session")
        for t in harness._drainers:
            t.join(timeout=5.0)
        return harness.stdout_rest.decode("utf-8", "replace"), harness.stderr.decode("utf-8", "replace")


# ---- Pass condition 4: the command line and the version ----

class CommandLineTest(unittest.TestCase):
    def run_exe(self, *args):
        return subprocess.run([exe_path(), *args], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=30)

    def test_an_invalid_log_format_is_a_usage_error(self):
        for args in (["--serve", "--headless", "--log-format", "xml"], ["--serve", "--headless", "--log-format"],
                     ["--serve", "--headless", "--log-format", "JSON"]):
            with self.subTest(args=args):
                r = self.run_exe(*args)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertIn(b"usage", r.stderr.lower())   # the usage text is plain, not JSON
                self.assertFalse(r.stderr.lstrip().startswith(b"{"))

    def test_both_log_formats_are_accepted(self):
        for fmt in ("text", "json"):
            with self.subTest(log_format=fmt):
                h = Harness.launch(headless=True, extra_args=["--log-format", fmt])
                try:
                    self.assertEqual(sorted(h.announcement), ["agent_token", "operator_token", "pid", "pipe"])
                finally:
                    h.close()


class HelloVersionTest(LoggingTestCase):
    def test_hello_reports_the_version_from_the_version_file(self):
        h = self.launch()
        op = h.connect("operator")
        self.assertEqual(op.hello_reply.get("server_version"), read_version())
        self.assertEqual(read_version(), "0.1.0")


# ---- Pass condition 4 / Review Focus 2: JSON lines on stderr ----

class JsonLogTest(LoggingTestCase):
    def work(self, harness):
        """Some work: a session, typing, a checkpoint and restore, a disk export, and a limit rejection."""
        op, agent = self.booted(harness)
        agent.call("type_text", text="10 REM " + TYPED_SECRET + "\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=TYPE_FRAMES, timeout=60.0)
        cp = op.call("checkpoint", name="log-test")
        op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        scratch = tempfile.mkdtemp(prefix="awh-m6-log-")
        self.addCleanup(shutil.rmtree, scratch, True)
        op.call("export_disk", slot=6, drive=1, dest_path=os.path.join(scratch, "out.dsk"))
        self.assertRpcError(-32602, agent, "press_key", key="A", hold_ms=10001)   # a limit rejection
        return op, agent

    def test_every_stderr_line_is_a_json_object_with_the_required_fields(self):
        h = self.launch_with(["--log-format", "json"])
        self.work(h)
        _, stderr = self.finish(h)
        lines = [l for l in stderr.splitlines() if l.strip()]
        self.assertGreaterEqual(len(lines), 4, stderr)   # startup, session, checkpoint/restore, the rejection...
        records = []
        for line in lines:
            with self.subTest(line=line[:120]):
                record = json.loads(line)
                self.assertIsInstance(record, dict)
                self.assertRegex(record["ts"], TS_RE)
                self.assertIn(record["level"], LEVELS)
                self.assertIsInstance(record["msg"], str)
                self.assertTrue(record["msg"])
                self.assertIn(record["component"], COMPONENTS)
                records.append(record)
        levels = {r["level"] for r in records}
        self.assertIn("info", levels)
        self.assertIn("warning", levels, "the limit rejection must be logged at warning")
        self.assertIn(read_version(), " ".join(json.dumps(r) for r in records), "the startup line carries the version")

    def test_no_token_or_typed_text_appears_in_stdout_stderr_or_log_notifications(self):
        for fmt in ("json", "text"):
            with self.subTest(log_format=fmt):
                h = self.launch_with(["--log-format", fmt])
                op, agent = self.work(h)
                notes = [n for n in op.drain_notifications(quiet=0.5, limit=5.0) if n.get("method") == "log"]
                stdout, stderr = self.finish(h)
                everything = stdout + "\n" + stderr + "\n" + json.dumps(notes)
                self.assertNotIn(h.agent_token, everything)
                self.assertNotIn(h.operator_token, everything)
                self.assertNotIn(TYPED_SECRET, everything)
                self.assertEqual(stdout.strip(), "", "stdout after the announcement line must stay empty")

    def test_the_announcement_line_is_unchanged_in_json_mode(self):
        h = self.launch_with(["--log-format", "json"])
        line = h.announcement_line.decode("ascii").rstrip("\r\n")
        self.assertRegex(line, r'^\{"pipe":"\\\\\\\\\.\\\\pipe\\\\applewin-harness-[0-9a-f]{32}",'
                               r'"agent_token":"[0-9a-f]{64}","operator_token":"[0-9a-f]{64}","pid":[1-9][0-9]*\}$')


class LogNotificationTest(LoggingTestCase):
    def test_operators_get_warnings_and_errors_only(self):
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", run_mode="stepped")
        agent = h.connect("agent")
        self.assertRpcError(-32602, agent, "press_key", key="A", hold_ms=10001)
        notes = [n for n in op.drain_notifications(quiet=0.5, limit=5.0) if n.get("method") == "log"]
        self.assertTrue(notes, "no log notification for a limit rejection")
        for n in notes:
            self.assertEqual(sorted(n["params"]), ["emulated_ms", "level", "message"])
            self.assertIn(n["params"]["level"], ("warning", "error"))
            self.assertIsInstance(n["params"]["message"], str)
            self.assertIsInstance(n["params"]["emulated_ms"], (int, float))

    def test_agents_get_no_log_notifications(self):
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", run_mode="stepped")
        agent = h.connect("agent")
        self.assertRpcError(-32602, agent, "press_key", key="A", hold_ms=10001)
        self.assertEqual([n for n in agent.drain_notifications(quiet=0.5, limit=3.0) if n.get("method") == "log"], [])


# ---- Pass condition 5: disk_written ----

class DiskWrittenTest(LoggingTestCase):
    def save(self, op, agent, name):
        agent.call("type_text", text="10 REM " + name + "\nSAVE " + name + "\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=TYPE_FRAMES, timeout=60.0)

    def written(self, op):
        return [n for n in op.drain_notifications(quiet=0.5, limit=5.0) if n.get("method") == "disk_written"]

    def test_save_sends_one_notification_per_write_burst(self):
        h = self.launch()
        op, agent = self.booted(h)
        self.assertEqual(op.call("subscribe_disk_writes"), {})
        self.save(op, agent, "FIRST")
        op.call("step_frames", frames=120, timeout=60.0)   # more than 1 emulated second without a write
        self.assertEqual([n["params"] for n in self.written(op)], [{"slot": 6, "drive": 1}])
        self.save(op, agent, "SECOND")
        op.call("step_frames", frames=120, timeout=60.0)
        self.assertEqual([n["params"] for n in self.written(op)], [{"slot": 6, "drive": 1}])   # a second burst

    def burst_ended_early_by(self, name, make_call, other_drive=False):
        """SAVE in stepped mode and stop 6-frame steps at the third change of the working copy: the head has moved
        off the last track written, so the burst is still open (the last write is under 1 emulated second old) and
        the SAVE has no more writes to come. `make_call(op, dest)` is the early end; it must report exactly one
        disk_written before its reply, and the quiet period after it must report none. With other_drive, `make_call`
        only touches drive 2: drive 1's burst goes on and ends once, at its quiet period, and drive 2 reports nothing."""
        h = self.launch()
        op, agent = self.booted(h)
        op.call("subscribe_disk_writes")
        working = op.call("list_disks")[0]["working_path"]
        seen = lambda: (os.stat(working).st_mtime_ns, os.stat(working).st_size)
        last, changes = seen(), 0
        agent.call("type_text", text="10 REM EARLY\nSAVE EARLY\n", inter_key_delay_ms=0)
        for _ in range(60):
            op.call("step_frames", frames=6, timeout=60.0)
            now = seen()
            changes += now != last
            last = now
            if changes == 3:
                break
        self.assertEqual(changes, 3, "the SAVE never wrote the disk three times")
        self.assertEqual(op.drain_notifications(quiet=0.2, limit=1.0), [], "the burst must still be open")
        make_call(op, os.path.join(self.tmp_dir(), name + ".dsk"))
        # The reader queues a notification before the reply that followed it on the wire, so one that was sent before
        # the reply is already here; none may arrive later.
        if other_drive:
            with self.assertRaises(TimeoutError):
                op.next_notification(timeout=0)
            op.call("step_frames", frames=120, timeout=60.0)
            self.assertEqual([n["params"] for n in self.written(op)], [{"slot": 6, "drive": 1}])
            return
        first = op.next_notification(timeout=0)
        self.assertEqual((first["method"], first["params"]), ("disk_written", {"slot": 6, "drive": 1}))
        op.call("step_frames", frames=120, timeout=60.0)   # more than 1 emulated second of quiet
        self.assertEqual(self.written(op), [], "a burst ended early must not be reported again at the quiet period")

    def tmp_dir(self):
        d = tempfile.mkdtemp(prefix="awh-dw-")
        self.addCleanup(shutil.rmtree, d, True)
        return d

    def test_export_disk_ends_the_burst_before_its_reply(self):
        self.burst_ended_early_by("export", lambda op, dest: op.call("export_disk", slot=6, drive=1, dest_path=dest))

    def test_checkpoint_ends_the_burst_before_its_reply(self):
        self.burst_ended_early_by("cp", lambda op, dest: op.call("checkpoint", name="early"))

    def test_insert_and_eject_on_the_other_drive_leave_an_open_burst_alone(self):
        def swap(op, dest):
            op.call("insert_disk", slot=6, drive=2, path=DOS_IMAGE)
            op.call("eject_disk", slot=6, drive=2)
        self.burst_ended_early_by("other", swap, other_drive=True)

    def test_eject_disk_ends_the_burst_before_its_reply(self):
        self.burst_ended_early_by("eject", lambda op, dest: op.call("eject_disk", slot=6, drive=1))

    def test_no_notification_without_a_subscription_or_after_unsubscribing(self):
        h = self.launch()
        op, agent = self.booted(h)
        self.save(op, agent, "FIRST")
        op.call("step_frames", frames=120, timeout=60.0)
        self.assertEqual(self.written(op), [])
        self.assertEqual(op.call("subscribe_disk_writes"), {})
        self.assertEqual(op.call("unsubscribe_disk_writes"), {})
        self.save(op, agent, "SECOND")
        op.call("step_frames", frames=120, timeout=60.0)
        self.assertEqual(self.written(op), [])

    def test_reading_the_disk_is_not_a_write(self):
        h = self.launch()
        op, agent = self.booted(h)
        op.call("subscribe_disk_writes")
        agent.call("type_text", text="CATALOG\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=TYPE_FRAMES + 120, timeout=60.0)
        self.assertEqual(self.written(op), [])


# ---- Pass condition 6: D10, a non-ASCII work path ----

def short_path(path):
    buf = ctypes.create_unicode_buffer(32768)
    n = ctypes.windll.kernel32.GetShortPathNameW(path, buf, len(buf))
    return buf.value if n else None


class NonAsciiWorkPathTest(LoggingTestCase):
    def test_checkpoint_and_restore_work_when_temp_has_a_short_ascii_name(self):
        base = tempfile.mkdtemp(prefix="awh-m6-")
        self.addCleanup(shutil.rmtree, base, True)
        temp = os.path.join(base, "témp-üñ-å")
        os.mkdir(temp)
        short = short_path(temp)
        if not short or not short.isascii() or short.lower() == temp.lower():
            self.skipTest("8.3 short names are disabled on this volume ({}): no ASCII path exists for {!r}".format(
                short, temp))
        h = self.launch_with(env={"TEMP": temp, "TMP": temp})
        op, _ = self.booted(h)
        listed = op.call("list_disks")
        self.assertEqual(len(listed), 1)
        self.assertTrue(listed[0]["working_path"].lower().startswith(temp.lower()),
                        "working_path must stay the long path: {!r}".format(listed[0]["working_path"]))
        cp = op.call("checkpoint", name="d10")
        before = op.call("get_state_checksum")["sha256"]
        op.call("step_frames", frames=30, timeout=60.0)
        op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        self.assertEqual(op.call("get_state_checksum")["sha256"], before)
        self.assertEqual(op.call("list_disks")[0]["working_path"], listed[0]["working_path"])


if __name__ == "__main__":
    unittest.main()
