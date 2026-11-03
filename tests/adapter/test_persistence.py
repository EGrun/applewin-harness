# SPDX-License-Identifier: MIT
"""Unit tests for the adapter's state directory and resume decision (docs/specs/persistence.md). No harness."""
import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "adapter"))

from unittest import mock  # noqa: E402

from applewin_mcp import persistence  # noqa: E402
from applewin_mcp.persistence import StateDir, StateDirLocked, resume_decision, utc_now  # noqa: E402


def sha(data):
    return hashlib.sha256(data).hexdigest()


class StateDirTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.state = os.path.join(self.root, "state")
        self.sd = StateDir(self.state)
        self.addCleanup(self.sd.close)

    def original(self, folder, name="Game.dsk", data=b"original"):
        d = os.path.join(self.root, folder)
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def index(self):
        with open(os.path.join(self.sd.disks_dir, "index.json")) as f:
            return json.load(f)

    def test_layout(self):
        for d in ("disks", "states", "archive"):
            self.assertTrue(os.path.isdir(os.path.join(self.state, d)))
        self.assertTrue(os.path.isabs(self.sd.disks_dir) and os.path.isabs(self.sd.states_dir))

    def test_disk_for_copies_the_original_once(self):
        orig = self.original("a")
        mounted = self.sd.disk_for(orig)
        self.assertEqual(mounted, os.path.join(self.sd.disks_dir, "Game.dsk"))
        with open(mounted, "rb") as f:
            self.assertEqual(f.read(), b"original")
        with open(mounted, "wb") as f:
            f.write(b"changed")
        self.assertEqual(self.sd.disk_for(orig), mounted)
        with open(mounted, "rb") as f:
            self.assertEqual(f.read(), b"changed")
        with open(orig, "rb") as f:
            self.assertEqual(f.read(), b"original")
        entries = self.index()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["key"], "Game.dsk")
        self.assertEqual(entries[0]["original_sha256"], sha(b"original"))
        for k in ("original_path", "first_used", "saved_at", "sha256"):
            self.assertIn(k, entries[0])

    def test_lookup_ignores_case(self):
        orig = self.original("a")
        first = self.sd.disk_for(orig)
        self.assertEqual(self.sd.disk_for(orig.upper()), first)

    def test_same_name_different_folders_get_suffixed_keys(self):
        keys = [os.path.basename(self.sd.disk_for(self.original(f))) for f in ("a", "b", "c")]
        self.assertEqual(keys, ["Game.dsk", "Game-2.dsk", "Game-3.dsk"])

    def test_a_path_inside_disks_is_mounted_as_given(self):
        mounted = self.sd.disk_for(self.original("a"))
        self.assertEqual(self.sd.disk_for(mounted), mounted)
        self.assertEqual(self.sd.key_of(mounted), "Game.dsk")
        self.assertIsNone(self.sd.key_of(self.original("b")))

    def test_a_changed_original_still_uses_the_copy_and_warns(self):
        orig = self.original("a")
        mounted = self.sd.disk_for(orig)
        with open(orig, "wb") as f:
            f.write(b"new original")
        with self.assertLogs("applewin_mcp", level="WARNING") as cm:
            again = self.sd.disk_for(orig)
        self.assertEqual(again, mounted)
        text = "\n".join(cm.output)
        self.assertIn(sha(b"original"), text)
        self.assertIn(sha(b"new original"), text)

    def test_save_replaces_the_copy_and_updates_the_index(self):
        mounted = self.sd.disk_for(self.original("a"))
        before = self.index()[0]
        seen = []

        def write_to(tmp):
            seen.append(os.path.exists(tmp))
            self.assertEqual(os.path.dirname(tmp), self.sd.disks_dir)
            with open(tmp, "wb") as f:
                f.write(b"saved")

        self.sd.save("Game.dsk", write_to)
        self.assertEqual(seen, [False])
        with open(mounted, "rb") as f:
            self.assertEqual(f.read(), b"saved")
        after = self.index()[0]
        self.assertEqual(after["sha256"], sha(b"saved"))
        self.assertNotEqual(after["sha256"], before["sha256"])
        self.assertGreaterEqual(after["saved_at"], before["saved_at"])
        self.assertEqual(sorted(os.listdir(self.sd.disks_dir)), ["Game.dsk", "index.json"])

    def test_a_failing_save_changes_nothing(self):
        mounted = self.sd.disk_for(self.original("a"))
        before = self.index()

        def write_to(tmp):
            with open(tmp, "wb") as f:
                f.write(b"partial")
            raise RuntimeError("boom")

        with self.assertRaises(RuntimeError):
            self.sd.save("Game.dsk", write_to)
        with open(mounted, "rb") as f:
            self.assertEqual(f.read(), b"original")
        self.assertEqual(self.index(), before)
        self.assertEqual(sorted(os.listdir(self.sd.disks_dir)), ["Game.dsk", "index.json"])

    def test_disk_hashes(self):
        self.sd.disk_for(self.original("a"))
        self.assertEqual(self.sd.disk_hashes(), {"Game.dsk": sha(b"original")})

    def test_disk_hashes_counts_only_indexed_copies(self):
        self.sd.disk_for(self.original("a"))
        for stray in ("desktop.ini", "Thumbs.db", "Game.tmp-abc.dsk", "dropped.dsk"):
            with open(os.path.join(self.sd.disks_dir, stray), "wb") as f:
                f.write(b"stray")
        self.assertEqual(self.sd.disk_hashes(), {"Game.dsk": sha(b"original")})

    def test_a_missing_copy_is_recopied_with_a_warning(self):
        orig = self.original("a")
        mounted = self.sd.disk_for(orig)
        os.remove(mounted)
        with self.assertLogs("applewin_mcp", level="WARNING") as cm:
            again = self.sd.disk_for(orig)
        self.assertEqual(again, mounted)
        self.assertIn("Game.dsk", "\n".join(cm.output))
        self.assertEqual(self.sd.disk_hashes(), {"Game.dsk": sha(b"original")})

    def test_replace_is_retried_when_the_file_is_busy(self):
        mounted = self.sd.disk_for(self.original("a"))
        real, calls = os.replace, []

        def flaky(src, dst):
            calls.append(src)
            if len(calls) <= 2:
                raise PermissionError(13, "busy", None, 32)
            real(src, dst)

        def write_to(tmp):
            with open(tmp, "wb") as f:
                f.write(b"saved")
        with mock.patch.object(persistence.os, "replace", flaky), mock.patch.object(persistence.time, "sleep"):
            self.sd.save("Game.dsk", write_to)
        self.assertGreaterEqual(len(calls), 3)
        with open(mounted, "rb") as f:
            self.assertEqual(f.read(), b"saved")

    def test_replace_gives_up_after_a_while(self):
        mounted = self.sd.disk_for(self.original("a"))

        def busy(src, dst):
            raise PermissionError(13, "busy", None, 32)

        def write_to(tmp):
            with open(tmp, "wb") as f:
                f.write(b"saved")
        with mock.patch.object(persistence.os, "replace", busy), mock.patch.object(persistence.time, "sleep"):
            with self.assertRaises(PermissionError):
                self.sd.save("Game.dsk", write_to)
        self.assertEqual(sorted(os.listdir(self.sd.disks_dir)), ["Game.dsk", "index.json"])
        with open(mounted, "rb") as f:
            self.assertEqual(f.read(), b"original")

    def test_a_hand_edited_index_is_cleaned_with_a_warning(self):
        orig = self.original("a")
        mounted = self.sd.disk_for(orig)
        good = self.index()[0]
        with open(os.path.join(self.sd.disks_dir, "index.json"), "w") as f:
            json.dump([good, "junk", 7, {"key": "Half.dsk"}, {"original_path": "x"}], f)
        self.sd.close()
        with self.assertLogs("applewin_mcp", level="WARNING") as cm:
            self.sd = StateDir(self.state)
        self.addCleanup(self.sd.close)
        self.assertEqual(len(cm.output), 4, cm.output)
        self.assertEqual(self.sd.disk_for(orig), mounted)
        self.assertEqual(self.sd.disk_hashes(), {"Game.dsk": sha(b"original")})
        self.sd.save("Game.dsk", lambda tmp: open(tmp, "wb").close())   # the next index write drops the junk
        self.assertEqual([e["key"] for e in self.index()], ["Game.dsk"])

    def test_saving_a_key_with_no_entry_adds_one(self):
        mounted = self.sd.disk_for(self.original("a"))
        with open(os.path.join(self.sd.disks_dir, "index.json"), "w") as f:
            f.write("[]")
        self.sd.close()
        self.sd = StateDir(self.state)
        self.addCleanup(self.sd.close)

        def write_to(tmp):
            with open(tmp, "wb") as f:
                f.write(b"saved")
        self.sd.save("Game.dsk", write_to)
        entries = self.index()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["key"], "Game.dsk")
        self.assertIsNone(entries[0]["original_path"])
        self.assertEqual(entries[0]["sha256"], sha(b"saved"))
        for k in ("first_used", "saved_at", "original_sha256"):
            self.assertIn(k, entries[0])
        # an entry with no original is never matched by a path, and its key is taken
        other = self.original("b")
        self.assertEqual(os.path.basename(self.sd.disk_for(other)), "Game-2.dsk")
        self.assertEqual(self.sd.disk_hashes()["Game.dsk"], sha(b"saved"))
        self.assertTrue(os.path.isfile(mounted))

    def test_a_moved_state_directory_still_finds_its_keys(self):
        self.sd.disk_for(self.original("a"))
        elsewhere = os.path.join(self.root, "old-state", "disks", "Game.dsk")
        self.assertEqual(self.sd.key_of_moved(elsewhere), "Game.dsk")
        self.assertIsNone(self.sd.key_of_moved(os.path.join(self.root, "old-state", "disks", "Other.dsk")))
        self.assertIsNone(self.sd.key_of_moved(os.path.join(self.root, "somewhere", "Game.dsk")))

    def test_utc_now_is_public(self):
        self.assertRegex(utc_now(), r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")

    def test_archive_disks_moves_the_copies_aside(self):
        orig = self.original("a")
        mounted = self.sd.disk_for(orig)
        with open(mounted, "wb") as f:
            f.write(b"played")
        archived = self.sd.archive_disks()
        self.assertEqual(os.path.dirname(archived), os.path.join(self.state, "archive"))
        self.assertRegex(os.path.basename(archived), r"^disks-\d{8}T\d{6}Z$")
        self.assertEqual(os.listdir(self.sd.disks_dir), [])
        with open(os.path.join(archived, "Game.dsk"), "rb") as f:
            self.assertEqual(f.read(), b"played")
        fresh = self.sd.disk_for(orig)
        with open(fresh, "rb") as f:
            self.assertEqual(f.read(), b"original")

    def test_a_second_state_dir_is_locked(self):
        with self.assertRaises(StateDirLocked):
            StateDir(self.state)
        self.sd.close()
        again = StateDir(self.state)
        again.close()

    def test_json_files_are_atomic_and_tolerant(self):
        self.assertIsNone(self.sd.read_json("last_session.json"))
        self.sd.write_json("last_session.json", {"a": 1})
        self.assertEqual(self.sd.read_json("last_session.json"), {"a": 1})
        with open(os.path.join(self.state, "startup.json"), "w") as f:
            f.write('{"a": ')
        self.assertIsNone(self.sd.read_json("startup.json"))
        self.assertEqual(sorted(n for n in os.listdir(self.state) if n.endswith(".tmp") or ".tmp" in n), [])


class ResumeDecisionTests(unittest.TestCase):
    def call(self, **over):
        args = dict(run_mode="stepped",
                    last_session={"session_id": "s1", "run_mode": "stepped", "ended_cleanly": True,
                                  "end_checkpoint_id": "cp-1", "disks": {"Game.dsk": "h"}},
                    previous_startup={"session_id": "s1"},
                    checkpoints=[{"checkpoint_id": "cp-1", "compatible": True}],
                    disk_hashes={"Game.dsk": "h"}, fresh_machine=False, fresh_disks=False)
        args.update(over)
        return resume_decision(**args)

    def test_resume_decision_reasons(self):
        ls = self.call()
        self.assertEqual(ls, ("cp-1", "restored"))
        base_ls = {"session_id": "s1", "run_mode": "stepped", "ended_cleanly": True,
                   "end_checkpoint_id": "cp-1", "disks": {"Game.dsk": "h"}}
        cases = [
            ("realtime", dict(run_mode="realtime")),
            ("fresh_machine", dict(fresh_machine=True)),
            ("fresh_disks", dict(fresh_disks=True)),
            ("first_session", dict(last_session=None)),
            ("previous_unclean", dict(previous_startup={"session_id": "s2"})),
            ("previous_unclean", dict(previous_startup=None)),
            ("previous_unclean", dict(last_session=dict(base_ls, ended_cleanly=False))),
            ("previous_realtime", dict(last_session=dict(base_ls, run_mode="realtime", end_checkpoint_id=None))),
            ("no_end_state", dict(last_session=dict(base_ls, end_checkpoint_id=None))),
            ("no_end_state", dict(checkpoints=[])),
            ("other_build", dict(checkpoints=[{"checkpoint_id": "cp-1", "compatible": False}])),
            ("disks_changed", dict(disk_hashes={"Game.dsk": "other"})),
        ]
        for reason, over in cases:
            with self.subTest(reason=reason, over=list(over)):
                self.assertEqual(self.call(**over), (None, reason))

    def test_order_when_several_apply(self):
        self.assertEqual(self.call(run_mode="realtime", fresh_machine=True, fresh_disks=True, last_session=None),
                         (None, "realtime"))
        self.assertEqual(self.call(fresh_machine=True, fresh_disks=True, last_session=None), (None, "fresh_machine"))
        self.assertEqual(self.call(fresh_disks=True, last_session=None), (None, "fresh_disks"))
        self.assertEqual(self.call(previous_startup=None, checkpoints=[], disk_hashes={}), (None, "previous_unclean"))


if __name__ == "__main__":
    unittest.main()
