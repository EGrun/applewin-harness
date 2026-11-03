# SPDX-License-Identifier: MIT
"""Checkpoints that persist (docs/specs/persistence.md 3.1-3.2, D18): create_session.checkpoint_dir, restoring a
checkpoint an earlier process made, and -32011 for one another build made.

Run: python -m unittest discover -s tests/python -p "test_persistent_checkpoints.py" -v   (HARNESS_EXE selects the exe).
"""

import glob
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "adapter"))   # applewin_mcp.harness_client

from applewin_mcp.harness_client import Harness, RpcError
from test_checkpoints import CheckpointTestCase, frame_hash, sha256_file, work_dir
from test_logging import short_path
from test_pipe import DOS_IMAGE, dos_disks

OTHER_BUILD = "0.0.0+000000000000"


class PersistentCheckpointTestCase(CheckpointTestCase):
    def setUp(self):
        self.cp_dir = tempfile.mkdtemp(prefix="awh-persist-")
        self.addCleanup(shutil.rmtree, self.cp_dir, True)

    def process(self, run_mode="stepped", boot=False, checkpoint_dir=True, env=None, disks=True):
        """A new harness process with a session on self.cp_dir (and the System Master in drive 1). Returns
        (harness, operator, agent). boot: 720 frames, so DOS has booted."""
        harness = Harness.launch(headless=True, env=env)
        self.addCleanup(harness.close)
        watchdog = threading.Timer(120.0, harness.kill)
        watchdog.daemon = True
        watchdog.start()
        self.addCleanup(watchdog.cancel)
        op = harness.connect("operator")
        params = {"run_mode": run_mode, "allow_text_screen": True}
        if disks:
            params["disks"] = dos_disks()
        if checkpoint_dir:
            params["checkpoint_dir"] = self.cp_dir
        op.call("create_session", timeout=30.0, **params)
        agent = harness.connect("agent")
        if boot:
            op.call("step_frames", frames=720, timeout=60.0)
        return harness, op, agent

    def screen(self, agent):
        return "\n".join(agent.call("get_text_screen")["lines"])

    def meta(self, cp="cp-1"):
        with open(os.path.join(self.cp_dir, cp, "meta.json"), encoding="utf-8") as f:
            return json.load(f)

    def first_checkpoint(self, name="first", type_text='10 PRINT "HI"\n'):
        """Process 1: boots, types a line, checkpoints, records the checksum and frame hash, closes. Returns them."""
        h, op, agent = self.process(boot=True)
        agent.call("type_text", text=type_text, inter_key_delay_ms=0)
        op.call("step_frames", frames=30)
        cp = op.call("checkpoint", name=name)
        self.assertEqual(cp["checkpoint_id"], "cp-1")
        checksum = op.call("get_state_checksum")
        frame = frame_hash(agent.call("get_frame", format="BGRA32"))
        h.close()
        return cp, checksum, frame


class PersistentCheckpointsTest(PersistentCheckpointTestCase):
    def test_a_checkpoint_survives_the_process(self):
        cp, checksum, frame = self.first_checkpoint()
        meta = self.meta()
        self.assertRegex(meta["build"], r"^\S+\+\S+$")
        self.assertEqual(meta["run_mode"], "stepped")
        self.assertEqual(len(meta["disks"]), 1)
        self.assertEqual(os.path.normcase(meta["disks"][0]["original_path"]), os.path.normcase(DOS_IMAGE))
        self.assertTrue(meta["disks"][0]["state_path"])

        h, op, agent = self.process()
        listed = op.call("list_checkpoints")
        self.assertEqual([(e["checkpoint_id"], e["name"], e["compatible"]) for e in listed], [("cp-1", "first", True)])
        self.assertEqual(op.call("restore_checkpoint", checkpoint_id="cp-1"), {"emulated_cycles": cp["emulated_cycles"]})
        self.assertEqual(op.call("get_state_checksum"), checksum)
        self.assertEqual(frame_hash(agent.call("get_frame", format="BGRA32")), frame)

        disks = op.call("list_disks")
        self.assertEqual(len(disks), 1)
        self.assertEqual(os.path.normcase(disks[0]["original_path"]), os.path.normcase(DOS_IMAGE))
        restore_disks = os.path.join(work_dir(h), "restore-1", "disks") + os.sep
        self.assertTrue(os.path.normcase(disks[0]["working_path"]).startswith(os.path.normcase(restore_disks)),
                        disks[0]["working_path"])
        out = os.path.join(self.scratch_dir(), "out.dsk")
        op.call("export_disk", slot=6, drive=1, dest_path=out)
        copies = glob.glob(os.path.join(self.cp_dir, "cp-1", "disk1-*"))
        self.assertEqual(len(copies), 1)
        self.assertEqual(sha256_file(out), sha256_file(copies[0]))

    def test_the_disks_come_back_as_they_were(self):
        h, op, agent = self.process(boot=True)
        cp = op.call("checkpoint", name="before save")
        agent.call("type_text", text='10 PRINT "ZED"\nSAVE ZEDPROG\nCATALOG\n', inter_key_delay_ms=0)
        op.call("step_frames", frames=400, timeout=60.0)
        self.assertIn("ZEDPROG", self.screen(agent), "the control: the saved program is listed in process 1")
        h.close()

        h, op, agent = self.process()
        op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        agent.call("type_text", text="CATALOG\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=200, timeout=60.0)
        screen = self.screen(agent)
        self.assertIn("DISK VOLUME", screen)
        self.assertNotIn("ZEDPROG", screen)

    def test_ids_continue_and_delete_removes_the_directory(self):
        self.first_checkpoint()
        h, op, _ = self.process()
        self.assertEqual(op.call("checkpoint")["checkpoint_id"], "cp-2")
        self.assertEqual(op.call("delete_checkpoint", checkpoint_id="cp-1"), {})
        self.assertFalse(os.path.exists(os.path.join(self.cp_dir, "cp-1")))
        h.close()
        h, op, _ = self.process()
        self.assertEqual([e["checkpoint_id"] for e in op.call("list_checkpoints")], ["cp-2"])
        self.assertEqual(op.call("checkpoint")["checkpoint_id"], "cp-3")

    def test_another_build_gets_32011_and_the_session_stays_usable(self):
        self.first_checkpoint()
        path = os.path.join(self.cp_dir, "cp-1", "meta.json")
        meta = self.meta()
        this_build = meta["build"]
        meta["build"] = OTHER_BUILD
        with open(path, "w", encoding="utf-8") as f:
            json.dump(meta, f)

        h, op, _ = self.process()
        self.assertEqual([(e["checkpoint_id"], e["compatible"]) for e in op.call("list_checkpoints")], [("cp-1", False)])
        with self.assertRaises(RpcError) as ctx:
            op.call("restore_checkpoint", checkpoint_id="cp-1")
        self.assertEqual(ctx.exception.code, -32011)
        self.assertIn(OTHER_BUILD, ctx.exception.message)
        self.assertIn(this_build, ctx.exception.message)
        op.call("step_frames", frames=3)
        cp = op.call("checkpoint")
        self.assertEqual(cp["checkpoint_id"], "cp-2")
        self.assertEqual([e["compatible"] for e in op.call("list_checkpoints")], [False, True])
        op.call("step_frames", frames=3)
        self.assertEqual(op.call("restore_checkpoint", checkpoint_id="cp-2"), {"emulated_cycles": cp["emulated_cycles"]})

    def test_leftovers_are_removed_and_broken_directories_skipped(self):
        self.first_checkpoint()
        os.makedirs(os.path.join(self.cp_dir, "cp-9.tmp"))
        with open(os.path.join(self.cp_dir, "cp-9.tmp", "state.yaml"), "w") as f:
            f.write("x")
        with open(os.path.join(self.cp_dir, "pending.yaml"), "w") as f:
            f.write("x")
        os.makedirs(os.path.join(self.cp_dir, "cp-5"))
        with open(os.path.join(self.cp_dir, "cp-5", "meta.json"), "w") as f:
            f.write("{}")

        h, op, _ = self.process()
        self.assertFalse(os.path.exists(os.path.join(self.cp_dir, "cp-9.tmp")))
        self.assertFalse(os.path.exists(os.path.join(self.cp_dir, "pending.yaml")))
        self.assertTrue(os.path.isdir(os.path.join(self.cp_dir, "cp-5")))
        self.assertEqual([e["checkpoint_id"] for e in op.call("list_checkpoints")], ["cp-1"])
        self.assertEqual(op.call("checkpoint")["checkpoint_id"], "cp-6")

    def test_a_second_process_on_the_directory_gets_32003(self):
        self.process()
        h = Harness.launch(headless=True)
        self.addCleanup(h.close)
        op = h.connect("operator")
        self.assertRpcError(-32003, op, "create_session", run_mode="stepped", checkpoint_dir=self.cp_dir)
        op.call("create_session", run_mode="stepped")

    def test_a_killed_process_releases_the_directory(self):
        h, _, _ = self.process()
        h.kill()
        h.process.wait(timeout=10)
        h2, op, _ = self.process()
        self.assertEqual(op.call("list_checkpoints"), [])

    def test_a_relative_checkpoint_dir_gets_32602(self):
        h = Harness.launch(headless=True)
        self.addCleanup(h.close)
        op = h.connect("operator")
        self.assertRpcError(-32602, op, "create_session", run_mode="stepped", checkpoint_dir="relative\\dir")
        self.assertRpcError(-32602, op, "create_session", run_mode="stepped", checkpoint_dir="\\no-drive")
        file_path = os.path.join(self.cp_dir, "a-file")
        with open(file_path, "w") as f:
            f.write("x")
        self.assertRpcError(-32602, op, "create_session", run_mode="stepped", checkpoint_dir=file_path)
        op.call("create_session", run_mode="stepped")

    def test_a_failed_restore_from_an_earlier_process_leaves_the_session_as_it_was(self):
        self.first_checkpoint()
        h, op, agent = self.process(boot=True)
        for copy in glob.glob(os.path.join(self.cp_dir, "cp-1", "disk1-*")):   # after create_session has read it
            os.remove(copy)
        disks = op.call("list_disks")
        checksum = op.call("get_state_checksum")
        frame = frame_hash(agent.call("get_frame", format="BGRA32"))
        self.assertRpcError(-32004, op, "restore_checkpoint", checkpoint_id="cp-1")
        self.assertEqual(op.call("list_disks"), disks)
        self.assertEqual(op.call("get_state_checksum"), checksum)
        self.assertEqual(frame_hash(agent.call("get_frame", format="BGRA32")), frame)
        self.assertFalse(os.path.exists(os.path.join(work_dir(h), "restore-1")))
        op.call("step_frames", frames=2)

    def test_a_failed_restore_after_the_copies_leaves_the_session_as_it_was(self):
        # The save state is damaged so that it no longer names the disk: the failure is found after the copies are made
        # and before anything is ejected.
        self.first_checkpoint()
        state = os.path.join(self.cp_dir, "cp-1", "state.yaml")
        with open(state, "rb") as f:
            text = f.read()
        with open(state, "wb") as f:
            f.write(text.replace(b"Absolute Path:", b"Absolute Pxth:"))
        h, op, agent = self.process(boot=True)
        disks = op.call("list_disks")
        checksum = op.call("get_state_checksum")
        self.assertRpcError(-32004, op, "restore_checkpoint", checkpoint_id="cp-1")
        self.assertEqual(op.call("list_disks"), disks)
        self.assertEqual(op.call("get_state_checksum"), checksum)
        self.assertFalse(os.path.exists(os.path.join(work_dir(h), "restore-1")))
        op.call("step_frames", frames=2)

    def test_a_checkpoint_after_a_restore_from_an_earlier_process(self):
        self.first_checkpoint()
        h, op, agent = self.process()
        op.call("restore_checkpoint", checkpoint_id="cp-1")
        agent.call("type_text", text='20 PRINT "TWO"\n', inter_key_delay_ms=0)
        op.call("step_frames", frames=30)
        self.assertEqual(op.call("checkpoint")["checkpoint_id"], "cp-2")
        checksum = op.call("get_state_checksum")
        agent.call("type_text", text='30 PRINT "THREE"\n', inter_key_delay_ms=0)
        op.call("step_frames", frames=30)
        op.call("restore_checkpoint", checkpoint_id="cp-2")
        self.assertEqual(op.call("get_state_checksum"), checksum)
        h.close()

        h, op, _ = self.process()
        op.call("restore_checkpoint", checkpoint_id="cp-2")
        self.assertEqual(op.call("get_state_checksum"), checksum)

    def test_restore_from_an_earlier_process_under_a_cjk_temp(self):
        _, checksum, _ = self.first_checkpoint()
        base = tempfile.mkdtemp(prefix="awh-persist-cjk-")
        self.addCleanup(shutil.rmtree, base, True)
        temp = os.path.join(base, "测试目录-checkpoint")
        os.mkdir(temp)
        short = short_path(temp)
        if not short or not short.isascii() or short.lower() == temp.lower():
            self.skipTest("8.3 short names are disabled on this volume ({}): no ASCII path exists for {!r}".format(
                short, temp))
        h, op, _ = self.process(env={"TEMP": temp, "TMP": temp})
        op.call("restore_checkpoint", checkpoint_id="cp-1")
        self.assertEqual(op.call("get_state_checksum"), checksum)
        disks = op.call("list_disks")
        self.assertTrue(disks[0]["working_path"].lower().startswith(temp.lower()), disks[0]["working_path"])

    def test_a_realtime_checkpoint_restores_in_a_later_stepped_session(self):
        h, op, _ = self.process(run_mode="realtime")
        cp = op.call("checkpoint", name="live")
        checksum = op.call("get_state_checksum")
        self.assertEqual(self.meta()["run_mode"], "realtime")
        h.close()

        h, op, _ = self.process()
        self.assertEqual(op.call("restore_checkpoint", checkpoint_id="cp-1"), {"emulated_cycles": cp["emulated_cycles"]})
        self.assertEqual(op.call("get_state_checksum"), checksum)
        op.call("step_frames", frames=2)


if __name__ == "__main__":
    unittest.main()
