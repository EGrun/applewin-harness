# SPDX-License-Identifier: MIT
"""A stepped session resumes where the last one ended (docs/specs/persistence.md §3.3 and §5; spec tests 11 to 14).

Run: python -m unittest discover -s tests/adapter -p test_resume.py -v   (HARNESS_EXE selects the exe).
Every test mounts a temporary copy of the DOS 3.3 image as the original and uses a fresh state directory.
"""

import asyncio
import json
import os
import re
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_disk_persistence import BURST_MS, PersistenceTestCase  # noqa: E402
from test_mcp import ADAPTER_DIR, CHECKPOINT_TOOLS, call, pid_alive, sha256_file, text_of  # noqa: E402
from applewin_mcp.harness_client import REPO_ROOT, exe_path  # noqa: E402

OTHER_BUILD = "0.0.0+aaaaaaaaaaaa"
SESSION_END = "session end"


class ResumeTestCase(PersistenceTestCase):
    def json_of(self, name):
        with open(os.path.join(self.state, name), encoding="utf-8") as f:
            return json.load(f)

    def startup(self):
        return self.json_of("startup.json")

    def last_session(self):
        return self.json_of("last_session.json")

    def states(self):
        return sorted(os.listdir(os.path.join(self.state, "states")))

    def set_build(self, checkpoint_id, build=OTHER_BUILD):
        path = os.path.join(self.state, "states", checkpoint_id, "meta.json")
        with open(path, encoding="utf-8") as f:
            meta = json.load(f)
        meta["build"] = build
        with open(path, "w", encoding="utf-8") as f:
            json.dump(meta, f)

    def clean_run(self, *more):
        """A stepped run that boots (or resumes) and closes; returns nothing, the files tell."""
        async def go(s):
            await call(s, "wait", ms=200)
        self.run_session(go, *more)

    def boot_and_print(self):
        async def go(s):
            await self.boot(s)
            await self.type_ok(s, "PRINT 1234\n")
        self.run_session(go)

    def realtime_run(self):
        async def go(s):
            await call(s, "wait", ms=200)
        self.run_session(go, "--realtime")

    def killed_run(self):
        """A stepped adapter started on its own and killed once startup.json is written: no clean shutdown."""
        before = (self.startup() or {}).get("session_id") if os.path.exists(os.path.join(self.state, "startup.json")) \
            else None
        env = dict(os.environ, PYTHONPATH=ADAPTER_DIR + os.pathsep + os.environ.get("PYTHONPATH", ""))
        command = [sys.executable, "-m", "applewin_mcp", "--exe", os.path.abspath(exe_path()), "--headless",
                   *self.args()]
        errlog = open(os.path.join(self.tmp, "killed.err"), "w+", encoding="utf-8")
        self.addCleanup(errlog.close)
        proc = subprocess.Popen(command, cwd=REPO_ROOT, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=errlog)
        self.addCleanup(proc.stdin.close)
        self.addCleanup(proc.stdout.close)
        try:
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    self.fail("the adapter exited early with {}".format(proc.returncode))
                path = os.path.join(self.state, "startup.json")
                if os.path.exists(path):
                    try:
                        if self.startup().get("session_id") != before:
                            break
                    except (OSError, ValueError):
                        pass
                time.sleep(0.25)
            else:
                self.fail("the adapter did not start")
        finally:
            proc.kill()
            proc.wait()
        errlog.seek(0)
        found = re.search(r"harness pid (\d+)", errlog.read())
        if found:   # the harness leaves when its pipes close; the next run needs its checkpoint lock released
            pid = int(found.group(1))
            deadline = time.monotonic() + 30
            while pid_alive(pid) and time.monotonic() < deadline:
                time.sleep(0.25)
            if pid_alive(pid):
                subprocess.run(["taskkill", "/F", "/PID", str(pid)], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
                time.sleep(1.0)


class ResumeTest(ResumeTestCase):
    def test_a_clean_stepped_shutdown_is_resumed(self):
        self.boot_and_print()
        last = self.last_session()
        first_id = self.startup()["session_id"]
        self.assertTrue(last["ended_cleanly"])
        self.assertEqual(last["session_id"], first_id)
        self.assertEqual(last["run_mode"], "stepped")
        self.assertTrue(last["end_checkpoint_id"])
        self.assertIn(last["end_checkpoint_id"], self.states())

        async def second(s):
            rows = text_of(await call(s, "list_checkpoints"))
            self.assertRegex(rows, r"{}\s+{}\s".format(re.escape(last["end_checkpoint_id"]), SESSION_END))
            self.assertIn("1234", await self.screen(s))   # nothing typed in this run
        self.run_session(second)
        started = self.startup()
        self.assertNotEqual(started["session_id"], first_id)
        self.assertEqual(started["restored"], last["end_checkpoint_id"])
        self.assertEqual(started["reason"], "restored")

    def test_no_restore_with_fresh_machine(self):
        self.boot_and_print()
        end = self.last_session()["end_checkpoint_id"]

        async def second(s):
            self.assertIn(end, text_of(await call(s, "list_checkpoints")))
            self.assertNotIn("1234", await self.screen(s))
        self.run_session(second, "--fresh-machine")
        self.assertIsNone(self.startup()["restored"])
        self.assertEqual(self.startup()["reason"], "fresh_machine")

    def test_no_restore_after_a_realtime_session(self):
        self.boot_and_print()
        self.realtime_run()
        self.assertEqual(self.startup()["reason"], "realtime")
        self.assertEqual(self.last_session()["run_mode"], "realtime")
        self.assertIsNone(self.last_session()["end_checkpoint_id"])
        self.clean_run()
        self.assertEqual(self.startup()["reason"], "previous_realtime")
        self.assertIsNone(self.startup()["restored"])

    def test_no_restore_after_a_killed_session(self):
        self.clean_run()
        self.killed_run()
        self.clean_run()
        self.assertEqual(self.startup()["reason"], "previous_unclean")
        self.assertIsNone(self.startup()["restored"])

    def test_no_restore_when_a_disk_changed(self):
        async def first(s):   # a SAVE, so the persisted copy differs from the original
            await self.boot(s)
            await self.save_program(s)
            await call(s, "wait", ms=BURST_MS)
        self.run_session(first)
        self.assertNotEqual(sha256_file(self.copy_of()), self.original_sha)
        with open(self.original, "rb") as f:
            original_bytes = f.read()
        with open(self.copy_of(), "wb") as f:
            f.write(original_bytes)
        self.clean_run()
        self.assertEqual(self.startup()["reason"], "disks_changed")
        self.assertIsNone(self.startup()["restored"])

    def test_no_restore_of_another_build(self):
        self.boot_and_print()
        self.set_build(self.last_session()["end_checkpoint_id"])
        self.clean_run()
        self.assertEqual(self.startup()["reason"], "other_build")
        self.assertIsNone(self.startup()["restored"])

    def test_only_five_session_ends_are_kept(self):
        async def first(s):
            made = await call(s, "checkpoint", name="mine")
            self.assertFalse(made.is_error, text_of(made))
        self.run_session(first)
        for _ in range(6):
            self.clean_run()

        async def last(s):
            rows = text_of(await call(s, "list_checkpoints")).splitlines()
            self.assertEqual(sum(1 for r in rows if SESSION_END in r), 5, rows)
            self.assertEqual(sum(1 for r in rows if "mine" in r), 1, rows)
            self.assertEqual(len(rows), 6, rows)
            self.assertIn(self.last_session()["end_checkpoint_id"], self.states())
        self.run_session(last)

    def test_session_end_is_reserved(self):
        async def go(s):
            refused = await call(s, "checkpoint", name=SESSION_END)
            self.assertTrue(refused.is_error)
            self.assertIn(SESSION_END, text_of(refused))
            self.assertEqual(text_of(await call(s, "list_checkpoints")), "no checkpoints")
        self.run_session(go)

    def test_realtime_takes_no_state(self):
        async def go(s):
            names = sorted(t.name for t in (await s.list_tools()).tools)
            for tool in CHECKPOINT_TOOLS:
                self.assertNotIn(tool, names)
            await call(s, "wait", ms=200)
        self.run_session(go, "--realtime")
        self.assertEqual(self.states(), [])
        self.assertEqual(self.startup()["reason"], "realtime")
        self.assertIsNone(self.last_session()["end_checkpoint_id"])

    def test_a_failed_start_writes_no_session_record(self):
        env = dict(os.environ, PYTHONPATH=ADAPTER_DIR + os.pathsep + os.environ.get("PYTHONPATH", ""))
        missing = os.path.join(self.tmp, "no-such.exe")
        command = [sys.executable, "-m", "applewin_mcp", "--exe", missing, "--headless", *self.args()]
        done = subprocess.run(command, cwd=REPO_ROOT, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, timeout=60)
        self.assertNotEqual(done.returncode, 0)
        self.assertFalse(os.path.exists(os.path.join(self.state, "last_session.json")))
        self.assertFalse(os.path.exists(os.path.join(self.state, "startup.json")))

        async def held(s):   # a start on a directory in use fails too, and leaves the owner's files alone
            command = [sys.executable, "-m", "applewin_mcp", "--exe", os.path.abspath(exe_path()), "--headless",
                       *self.args()]
            again = await asyncio.to_thread(subprocess.run, command, cwd=REPO_ROOT, env=env, stdin=subprocess.DEVNULL,
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)
            self.assertNotEqual(again.returncode, 0)
            self.assertFalse(os.path.exists(os.path.join(self.state, "last_session.json")))
        self.run_session(held)

    def test_list_checkpoints_marks_another_builds(self):
        self.boot_and_print()
        end = self.last_session()["end_checkpoint_id"]
        self.set_build(end)

        async def go(s):
            rows = text_of(await call(s, "list_checkpoints")).splitlines()
            row = next(r for r in rows if r.startswith(end))
            self.assertIn("made by another build; cannot be restored", row)
        self.run_session(go)


if __name__ == "__main__":
    unittest.main()
