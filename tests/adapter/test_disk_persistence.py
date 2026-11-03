# SPDX-License-Identifier: MIT
"""The adapter keeps every disk the machine writes between runs (docs/specs/persistence.md §2 and §4).

Run: python -m unittest discover -s tests/adapter -p test_disk_persistence.py -v   (HARNESS_EXE selects the exe).
Every test mounts a temporary copy of the DOS 3.3 image as the original and checks that the original never changes.
"""

import asyncio
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_mcp import (ADAPTER_DIR, DOS_IMAGE, AdapterTestCase, call, mcp_session, sha256_file,  # noqa: E402
                      text_of)
from applewin_mcp.harness_client import REPO_ROOT, exe_path  # noqa: E402

BOOT_MS = 12000    # boot DOS 3.3
BURST_MS = 8000    # SAVE takes a few emulated seconds, then the write burst ends about one second after the last write


def squeeze(text):
    return " ".join(text.split())


class PersistenceTestCase(AdapterTestCase):
    def setUp(self):
        super().setUp()
        self.tmp = tempfile.mkdtemp(prefix="awh-persist-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.state = os.path.join(self.tmp, "state")
        self.original = self.make_original("orig", "DOS.dsk")
        self.original_sha = sha256_file(self.original)

    def make_original(self, folder, name):
        os.makedirs(os.path.join(self.tmp, folder), exist_ok=True)
        path = os.path.join(self.tmp, folder, name)
        shutil.copyfile(DOS_IMAGE, path)
        return path

    def tearDown(self):
        self.assertEqual(sha256_file(self.original), self.original_sha, "the original image was written")

    def args(self, *more, state=True):
        out = ["--disk", self.original]
        if state:
            out += ["--state-dir", self.state]
        return out + list(more)

    def run_session(self, script, *more, **kw):
        async def go():
            async with mcp_session(self.errlog, extra_args=self.args(*more, **kw)) as s:
                return await script(s)
        return self.run_async(go)

    def copy_of(self, name="DOS.dsk"):
        return os.path.join(self.state, "disks", name)

    async def boot(self, s):
        await call(s, "wait", ms=BOOT_MS)

    async def type_ok(self, s, text):
        typed = await call(s, "type_text", text=text)
        self.assertFalse(typed.is_error, text_of(typed))

    async def screen(self, s):
        return squeeze(text_of(await call(s, "read_screen")))

    async def save_program(self, s, name="KEPT", text='10 PRINT "KEPT"'):
        await self.type_ok(s, text + "\n")
        await self.type_ok(s, "SAVE {}\n".format(name))

    async def load_and_list(self, s, name="KEPT"):
        """The screen after LOAD name and LIST (a missing file shows FILE NOT FOUND instead of the program)."""
        await self.type_ok(s, "LOAD {}\n".format(name))
        await call(s, "wait", ms=6000)
        await self.type_ok(s, "LIST\n")
        return await self.screen(s)

    def wait_for_change(self, path, before, seconds=30.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if sha256_file(path) != before:
                return True
            time.sleep(0.25)
        return False


class DiskPersistenceTest(PersistenceTestCase):
    def check_survives(self, *more):
        async def first(s):
            await self.boot(s)
            await self.save_program(s)
            await call(s, "wait", ms=BURST_MS)
        self.run_session(first, *more)
        self.assertNotEqual(sha256_file(self.copy_of()), self.original_sha)

        async def second(s):
            await self.boot(s)
            self.assertIn('10 PRINT "KEPT"', await self.load_and_list(s))
        self.run_session(second, *more)

    def test_a_saved_program_survives_the_adapter(self):
        self.check_survives()

    def test_the_same_in_a_realtime_session(self):
        self.check_survives("--realtime")

    def test_realtime_write_is_saved_without_a_tool_call(self):
        async def go(s):
            await self.boot(s)
            before = sha256_file(self.copy_of())
            await self.save_program(s)
            # no tool call from here: the adapter must save on the harness's disk_written alone
            self.assertTrue(await asyncio.to_thread(self.wait_for_change, self.copy_of(), before),
                            "the copy was not saved without a tool call")
        self.run_session(go, "--realtime")

    def test_insert_write_and_swap_back(self):
        second = self.make_original("other", "Second.dsk")
        second_sha = sha256_file(second)

        async def first(s):
            await self.boot(s)
            await self.save_program(s, "AONE", '10 PRINT "A"')
            await call(s, "wait", ms=BURST_MS)
            inserted = await call(s, "insert_disk", drive=1, path=second)
            self.assertFalse(inserted.is_error, text_of(inserted))
            await self.save_program(s, "BTWO", '10 PRINT "B"')
            await call(s, "wait", ms=BURST_MS)
            back = await call(s, "insert_disk", drive=1, path=self.original)
            self.assertFalse(back.is_error, text_of(back))
            self.assertIn(self.original, text_of(back))   # the reply names the path the agent gave
        self.run_session(first)
        self.assertNotEqual(sha256_file(self.copy_of()), self.original_sha)
        self.assertNotEqual(sha256_file(self.copy_of("Second.dsk")), second_sha)

        async def again(s):
            await self.boot(s)
            self.assertIn('10 PRINT "A"', await self.load_and_list(s, "AONE"))
            await call(s, "insert_disk", drive=1, path=second)
            self.assertIn('10 PRINT "B"', await self.load_and_list(s, "BTWO"))
        self.run_session(again)
        self.assertEqual(sha256_file(second), second_sha)

    def test_fresh_disks_archives_and_starts_from_the_originals(self):
        async def first(s):
            await self.boot(s)
            await self.save_program(s)
            await call(s, "wait", ms=BURST_MS)
        self.run_session(first)
        saved_sha = sha256_file(self.copy_of())
        self.assertNotEqual(saved_sha, self.original_sha)

        async def second(s):
            await self.boot(s)
            self.assertIn("FILE NOT FOUND", await self.load_and_list(s))
        self.run_session(second, "--fresh-disks")
        archived = glob.glob(os.path.join(self.state, "archive", "disks-*", "DOS.dsk"))
        self.assertEqual(len(archived), 1, archived)
        self.assertEqual(sha256_file(archived[0]), saved_sha)

    def test_same_name_originals_get_different_keys(self):
        other = self.make_original("other", "DOS.dsk")

        async def go(s):
            inserted = await call(s, "insert_disk", drive=2, path=other)
            self.assertFalse(inserted.is_error, text_of(inserted))
        self.run_session(go)
        self.assertEqual(sorted(n for n in os.listdir(os.path.join(self.state, "disks")) if n.endswith(".dsk")),
                         ["DOS-2.dsk", "DOS.dsk"])

    def test_the_same_original_in_both_drives_is_refused(self):
        async def go(s):
            refused = await call(s, "insert_disk", drive=2, path=self.original)
            self.assertTrue(refused.is_error)
            self.assertIn("already in drive 1", text_of(refused))
        self.run_session(go)

    def test_restore_saves_the_restored_disks(self):
        async def go(s):
            await self.boot(s)
            at_checkpoint = sha256_file(self.copy_of())
            made = await call(s, "checkpoint")
            self.assertFalse(made.is_error, text_of(made))
            await self.save_program(s)
            await call(s, "wait", ms=BURST_MS)
            # the watcher saves once the wait (which holds the machine's lock) returns
            self.assertTrue(await asyncio.to_thread(self.wait_for_change, self.copy_of(), at_checkpoint, 10.0))
            restored = await call(s, "restore_checkpoint", checkpoint_id="cp-1")
            self.assertFalse(restored.is_error, text_of(restored))
            self.assertEqual(sha256_file(self.copy_of()), at_checkpoint)
        self.run_session(go)


class StateDirectoryTest(PersistenceTestCase):
    def run_adapter(self, *more, timeout=60):
        env = dict(os.environ, PYTHONPATH=ADAPTER_DIR + os.pathsep + os.environ.get("PYTHONPATH", ""))
        command = [sys.executable, "-m", "applewin_mcp", "--exe", os.path.abspath(exe_path()), "--headless", *more]
        return subprocess.run(command, cwd=REPO_ROOT, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, timeout=timeout)

    def test_a_second_adapter_on_the_directory_exits(self):
        async def go(s):
            done = await asyncio.to_thread(self.run_adapter, "--disk", self.original, "--state-dir", self.state)
            self.assertNotEqual(done.returncode, 0)
            self.assertIn(self.state, done.stderr)
        self.run_session(go)

    def test_without_state_dir_nothing_is_kept(self):
        async def first(s):
            await self.boot(s)
            await self.save_program(s)
            await call(s, "wait", ms=BURST_MS)
        self.run_session(first, state=False)

        async def second(s):
            await self.boot(s)
            self.assertIn("FILE NOT FOUND", await self.load_and_list(s))
        self.run_session(second, state=False)
        self.assertFalse(os.path.exists(self.state))

    def test_fresh_flags_need_state_dir(self):
        for flag in ("--fresh-disks", "--fresh-machine"):
            with self.subTest(flag=flag):
                done = self.run_adapter("--disk", self.original, flag)
                self.assertEqual(done.returncode, 2, done.stderr)
                self.assertIn("--state-dir", done.stderr)


if __name__ == "__main__":
    unittest.main()
