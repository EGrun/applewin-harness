"""insert_disk and eject_disk over the pipe (spec: docs/specs/m5.md "Disk swapping"; plan Review Focus 6 and 7, Task 5).

Run: python -m unittest discover -s tests/python -v   (HARNESS_EXE selects the exe; default bin/x64/Debug).
"""

import hashlib
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "adapter"))   # applewin_mcp.harness_client

from applewin_mcp.harness_client import Harness
from test_pipe import DOS_IMAGE, PipeTestCase

UNIQUE_NAME = "QZXFIXTURE"
BOOT_FRAMES = 720
TYPE_FRAMES = 240          # frames for a typed line and a CATALOG or SAVE to finish
TEMP_DIR = None            # a tempfile directory for the module, removed in tearDownModule
FIXTURE = None             # a DOS 3.3 disk holding the Applesoft program UNIQUE_NAME


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def dos_name(name):
    """A DOS 3.3 catalog entry stores its name with the high bit set."""
    return bytes(ord(c) | 0x80 for c in name)


def build_fixture(dest):
    """Boots a copy of the master, SAVEs UNIQUE_NAME and exports the disk to dest."""
    h = Harness.launch(headless=True)
    watchdog = threading.Timer(120.0, h.kill)
    watchdog.daemon = True
    watchdog.start()
    try:
        op = h.connect("operator")
        op.call("create_session", run_mode="stepped", disks=[{"slot": 6, "drive": 1, "path": DOS_IMAGE}])
        agent = h.connect("agent")
        op.call("step_frames", frames=BOOT_FRAMES, timeout=60.0)
        agent.call("type_text", text="10 REM FIXTURE\nSAVE " + UNIQUE_NAME + "\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=TYPE_FRAMES * 2, timeout=60.0)
        op.call("export_disk", slot=6, drive=1, dest_path=dest)
    finally:
        watchdog.cancel()
        h.close()
    with open(dest, "rb") as f:
        if dos_name(UNIQUE_NAME) not in f.read():
            raise RuntimeError("the fixture disk does not hold " + UNIQUE_NAME)


def setUpModule():
    global TEMP_DIR, FIXTURE
    TEMP_DIR = tempfile.mkdtemp(prefix="awh-m5-disks-")
    FIXTURE = os.path.join(TEMP_DIR, "fixture.dsk")
    try:
        build_fixture(FIXTURE)
    except BaseException:
        shutil.rmtree(TEMP_DIR, True)
        raise


def tearDownModule():
    shutil.rmtree(TEMP_DIR, True)


class DiskSwapTestCase(PipeTestCase):
    def launch(self, headless=True):
        harness = Harness.launch(headless=headless)
        self.addCleanup(harness.close)
        watchdog = threading.Timer(120.0, harness.kill)
        watchdog.daemon = True
        watchdog.start()
        self.addCleanup(watchdog.cancel)
        return harness

    def booted(self, harness, disks=None, run_mode="stepped"):
        """A session (default: the master in drive 1) that has booted DOS 3.3, with text-screen reads allowed."""
        op = harness.connect("operator")
        op.call("create_session", run_mode=run_mode, allow_text_screen=True,
                disks=[{"slot": 6, "drive": 1, "path": DOS_IMAGE}] if disks is None else disks)
        agent = harness.connect("agent")
        self.advance(op, BOOT_FRAMES, run_mode)
        return op, agent

    def advance(self, op, frames, run_mode="stepped"):
        if run_mode == "stepped":
            op.call("step_frames", frames=frames, timeout=60.0)
        else:
            time.sleep(frames / 60.0)

    def screen(self, agent):
        return "\n".join(agent.call("get_text_screen")["lines"])

    def catalog(self, op, agent, command="CATALOG", run_mode="stepped"):
        """The screen after the whole disk has been listed. DOS 3.3 stops after 18 files and waits for a key (the
        master has 19, the fixture 20), so a space is typed to reach the end of the listing. The last page still
        shows HELLO at the top and ends with SLOT# (master) or QZXFIXTURE (fixture), which tells the disks apart."""
        agent.call("type_text", text="HOME\n" + command + "\n", inter_key_delay_ms=0)
        self.advance(op, TYPE_FRAMES, run_mode)
        agent.call("type_text", text=" ", inter_key_delay_ms=0)
        self.advance(op, TYPE_FRAMES // 2, run_mode)
        return self.screen(agent)

    def assertMasterCatalog(self, screen):
        self.assertIn("HELLO", screen)
        self.assertIn("SLOT#", screen)   # the master's last file: a blank or stale screen fails
        self.assertNotIn(UNIQUE_NAME, screen)

    def assertFixtureCatalog(self, screen):
        self.assertIn("HELLO", screen)
        self.assertIn(UNIQUE_NAME, screen)

    def drives(self, op):
        """{drive: original_path} for the occupied drives."""
        return {d["drive"]: d["original_path"] for d in op.call("list_disks")}

    def assertSamePath(self, a, b):
        self.assertEqual(os.path.normcase(os.path.realpath(a)), os.path.normcase(os.path.realpath(b)))


class ErrorCodeTest(DiskSwapTestCase):
    def test_insert_disk_errors(self):
        h = self.launch()
        op, _ = self.booted(h)
        self.assertRpcError(-32602, op, "insert_disk", slot=6, drive=2, path="relative.dsk")
        self.assertRpcError(-32005, op, "insert_disk", slot=6, drive=2, path=os.path.join(TEMP_DIR, "missing.dsk"))
        self.assertRpcError(-32602, op, "insert_disk", slot=5, drive=2, path=FIXTURE)
        self.assertRpcError(-32602, op, "insert_disk", slot=6, drive=3, path=FIXTURE)
        self.assertRpcError(-32602, op, "insert_disk", slot=6, drive=2)
        self.assertEqual(sorted(self.drives(op)), [1])   # none of the failures changed a drive

    def test_insert_disk_of_an_unmountable_image_leaves_the_drive_as_it_was(self):
        h = self.launch()
        op, _ = self.booted(h)
        junk = os.path.join(TEMP_DIR, "junk.dsk")
        with open(junk, "wb") as f:
            f.write(b"not a disk image")
        before = op.call("list_disks")
        self.assertRpcError(-32005, op, "insert_disk", slot=6, drive=1, path=junk)
        self.assertEqual(op.call("list_disks"), before)

    def test_eject_disk_errors(self):
        h = self.launch()
        op, _ = self.booted(h)
        self.assertRpcError(-32005, op, "eject_disk", slot=6, drive=2)   # empty
        self.assertRpcError(-32602, op, "eject_disk", slot=5, drive=1)
        self.assertRpcError(-32602, op, "eject_disk", slot=6, drive=3)
        self.assertRpcError(-32602, op, "eject_disk", slot=6, drive=0)
        self.assertEqual(sorted(self.drives(op)), [1])

    def test_a_second_eject_of_the_same_drive_is_an_error(self):
        h = self.launch()
        op, _ = self.booted(h)
        self.assertEqual(op.call("eject_disk", slot=6, drive=1), {})
        self.assertRpcError(-32005, op, "eject_disk", slot=6, drive=1)

    def test_no_session_gets_32002(self):
        h = self.launch()
        op = h.connect("operator")
        self.assertRpcError(-32002, op, "insert_disk", slot=6, drive=1, path=FIXTURE)
        self.assertRpcError(-32002, op, "eject_disk", slot=6, drive=1)

    def test_agent_role_gets_32001(self):
        h = self.launch()
        _, agent = self.booted(h)
        self.assertRpcError(-32001, agent, "insert_disk", slot=6, drive=2, path=FIXTURE)
        self.assertRpcError(-32001, agent, "eject_disk", slot=6, drive=1)

    def test_after_a_failed_restore_both_get_32008(self):
        h = self.launch()
        op, _ = self.booted(h)
        cp = op.call("checkpoint")
        state = os.path.join(tempfile.gettempdir(), "applewin-harness", str(h.pid), "checkpoints",
                             cp["checkpoint_id"], "state.yaml")
        with open(state, "rb") as f:
            data = f.read()
        cut = data.index(b"\n", len(data) * 2 // 5) + 1   # as test_checkpoints: the load starts, then fails
        with open(state, "wb") as f:
            f.write(data[:cut] + b"  ]]] not: [yaml\n" + data[cut:])
        self.assertRpcError(-32004, op, "restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        self.assertRpcError(-32008, op, "insert_disk", slot=6, drive=2, path=FIXTURE)
        self.assertRpcError(-32008, op, "eject_disk", slot=6, drive=1)


class SwapTest(DiskSwapTestCase):
    def test_list_disks_follows_every_swap(self):
        h = self.launch()
        op, _ = self.booted(h)
        self.assertEqual(self.drives(op), {1: DOS_IMAGE})
        self.assertEqual(op.call("insert_disk", slot=6, drive=2, path=FIXTURE), {})
        listed = {d["drive"]: d for d in op.call("list_disks")}
        self.assertEqual(sorted(listed), [1, 2])
        self.assertSamePath(listed[2]["original_path"], FIXTURE)
        self.assertFalse(listed[2]["write_protected"])
        self.assertNotEqual(os.path.normcase(listed[2]["working_path"]), os.path.normcase(FIXTURE))   # a copy is mounted
        self.assertTrue(os.path.exists(listed[2]["working_path"]))
        self.assertEqual(op.call("eject_disk", slot=6, drive=1), {})
        self.assertEqual(sorted(self.drives(op)), [2])
        self.assertEqual(op.call("eject_disk", slot=6, drive=2), {})
        self.assertEqual(op.call("list_disks"), [])

    def test_write_protected_is_reported(self):
        h = self.launch()
        op, _ = self.booted(h)
        op.call("insert_disk", slot=6, drive=2, path=FIXTURE, write_protected=True)
        listed = {d["drive"]: d for d in op.call("list_disks")}
        self.assertTrue(listed[2]["write_protected"])

    def test_insert_replaces_a_disk_already_in_the_drive(self):
        h = self.launch()
        op, agent = self.booted(h)
        self.assertMasterCatalog(self.catalog(op, agent))
        self.assertEqual(op.call("insert_disk", slot=6, drive=1, path=FIXTURE), {})   # no eject first
        self.assertEqual(len(op.call("list_disks")), 1)
        self.assertSamePath(self.drives(op)[1], FIXTURE)
        self.assertFixtureCatalog(self.catalog(op, agent))

    def test_export_disk_exports_the_disk_now_in_the_drive(self):
        h = self.launch()
        op, _ = self.booted(h)
        op.call("insert_disk", slot=6, drive=1, path=FIXTURE)
        out = os.path.join(TEMP_DIR, "exported.dsk")
        self.addCleanup(lambda: os.path.exists(out) and os.remove(out))
        op.call("export_disk", slot=6, drive=1, dest_path=out)
        with open(out, "rb") as f:
            self.assertIn(dos_name(UNIQUE_NAME), f.read())

    def test_originals_are_unchanged_byte_for_byte(self):
        master_before, fixture_before = sha256_file(DOS_IMAGE), sha256_file(FIXTURE)
        h = self.launch()
        op, agent = self.booted(h)
        op.call("insert_disk", slot=6, drive=2, path=FIXTURE)
        op.call("insert_disk", slot=6, drive=1, path=FIXTURE)   # replaces the master the session booted from
        op.call("insert_disk", slot=6, drive=2, path=DOS_IMAGE)
        # Write to both mounted disks: a BASIC program SAVEd to each.
        agent.call("type_text", text="10 REM CHANGED\nSAVE CHANGED1,D1\nSAVE CHANGED2,D2\n", inter_key_delay_ms=0)
        self.advance(op, TYPE_FRAMES * 2)
        op.call("eject_disk", slot=6, drive=1)
        op.call("eject_disk", slot=6, drive=2)
        op.call("close_session")
        h.wait_exit(10.0)
        self.assertEqual(sha256_file(DOS_IMAGE), master_before)
        self.assertEqual(sha256_file(FIXTURE), fixture_before)

    def test_swap_works_in_a_realtime_session(self):
        """Plan Review Focus 7: a swap while the machine runs leaves list_disks consistent and the session usable."""
        h = self.launch()
        op, agent = self.booted(h, run_mode="realtime")
        op.call("resume")
        time.sleep(0.3)
        self.assertEqual(op.call("insert_disk", slot=6, drive=1, path=FIXTURE), {})
        self.assertEqual(op.call("insert_disk", slot=6, drive=2, path=DOS_IMAGE), {})
        self.assertEqual(op.call("eject_disk", slot=6, drive=2), {})
        drives = self.drives(op)
        self.assertEqual(sorted(drives), [1])
        self.assertSamePath(drives[1], FIXTURE)
        self.assertFixtureCatalog(self.catalog(op, agent, run_mode="realtime"))
        self.assertGreater(op.call("get_state_checksum")["emulated_cycles"], 0)   # still answering

    def test_swap_in_a_stepped_session_does_not_pass_time(self):
        h = self.launch()
        op, _ = self.booted(h)
        before = op.call("get_state_checksum")
        op.call("insert_disk", slot=6, drive=2, path=FIXTURE)
        op.call("eject_disk", slot=6, drive=2)
        after = op.call("get_state_checksum")
        self.assertEqual(after["emulated_cycles"], before["emulated_cycles"])   # no pause, reset or time passing


class CheckpointAcrossSwapTest(DiskSwapTestCase):
    def test_restore_brings_back_the_old_drives_including_an_empty_drive_2(self):
        """Plan Review Focus 6."""
        h = self.launch()
        op, agent = self.booted(h)   # drive 1: the master; drive 2: empty
        cp = op.call("checkpoint")
        op.call("insert_disk", slot=6, drive=1, path=FIXTURE)
        op.call("insert_disk", slot=6, drive=2, path=FIXTURE)
        self.assertEqual(sorted(self.drives(op)), [1, 2])
        op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        drives = self.drives(op)
        self.assertEqual(sorted(drives), [1])   # drive 2 is empty again
        self.assertSamePath(drives[1], DOS_IMAGE)
        self.assertMasterCatalog(self.catalog(op, agent))   # the master, not a blank screen
        self.assertRpcError(-32005, op, "eject_disk", slot=6, drive=2)

    def test_restore_puts_back_a_disk_ejected_since_the_checkpoint(self):
        h = self.launch()
        op, agent = self.booted(h, disks=[{"slot": 6, "drive": 1, "path": DOS_IMAGE},
                                          {"slot": 6, "drive": 2, "path": FIXTURE}])
        cp = op.call("checkpoint")
        op.call("eject_disk", slot=6, drive=2)
        op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        drives = self.drives(op)
        self.assertEqual(sorted(drives), [1, 2])
        self.assertSamePath(drives[2], FIXTURE)
        self.assertFixtureCatalog(self.catalog(op, agent, "CATALOG,D2"))

    def test_checkpoint_records_the_drives_as_they_are_after_a_swap(self):
        h = self.launch()
        op, agent = self.booted(h)
        op.call("insert_disk", slot=6, drive=1, path=FIXTURE)
        cp = op.call("checkpoint")
        op.call("insert_disk", slot=6, drive=1, path=DOS_IMAGE)
        op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        self.assertSamePath(self.drives(op)[1], FIXTURE)
        self.assertFixtureCatalog(self.catalog(op, agent))


if __name__ == "__main__":
    unittest.main()
