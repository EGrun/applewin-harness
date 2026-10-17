"""M4 checkpoints over the pipe (spec: docs/specs/m4.md, pass conditions 1-5, "Restore purity" and "API"; plan Review
Focus 1, 3, 4 and 5).

Run: python -m unittest discover -s tests/python -v   (HARNESS_EXE selects the exe; default bin/x64/Debug).
Windowed tests are skipped unless HARNESS_WINDOWED=1.
"""

import base64
import hashlib
import os
import re
import shutil
import tempfile
import threading
import time
import unittest

from harness_client import Harness
from test_pipe import (CYCLES_PER_FRAME, BASE_CLOCK_HZ, DOS_IMAGE, WINDOWED, PipeTestCase, dos_disks, ms_to_cycles,
                       mono_ac_rms_db, text_row_band)

MAX_STEP_CYCLES = 17030000
ISO_UTC = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d+)?Z$")
# The SSI263 speech program of pass condition 5 is written in M4 Task 4 (it depends on what AppleWin's default slot-4
# card provides). Until then the speech tests fail with this message.
SSI263_PROGRAM = None
SSI263_MISSING = "SSI263 program not yet written (Task 4)"
# Ruling D6: prints any-key-down ($C010 bit 7), buttons 0 and 1 ($C061, $C062), PDL(0) and PDL(1), so input state leaked
# across a restore shows on screen and in RAM.
PRINT_INPUT_STATE = 'PRINT PEEK(-16368);" ";PEEK(-16287);" ";PEEK(-16286);" ";PDL(0);" ";PDL(1)\n'
# Disk II motor on ($C0E9); 20 x phases 3..0 on/off (head out to track 0); 30000 speaker toggles ($C030), ~30 s of tone.
BUSY_PROGRAM = ("NEW\n"
                "10 X=PEEK(49385)\n"
                "20 FOR T=1 TO 20:FOR P=3 TO 0 STEP -1:X=PEEK(49377+2*P):X=PEEK(49376+2*P):NEXT:NEXT\n"
                "30 FOR I=1 TO 30000:X=PEEK(49200):NEXT\n"
                "RUN\n")
SOUND_EVENTS = ("sound_started", "sound_stopped")


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def frame_hash(frame):
    return hashlib.sha256(base64.b64decode(frame["data"])).hexdigest()


def same_path(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def work_dir(harness):
    """The session's work directory, %TEMP%\\applewin-harness\\<pid>."""
    return os.path.join(tempfile.gettempdir(), "applewin-harness", str(harness.pid))


def run_script(op, agent, from_cycle):
    """Pass condition 1's script: type_text mixed with step_frames and step_cycles, a bell for audio, and a short disk
    read (CATALOG; ruling D4: both runs start from a restore, which reseeds rand() identically, D2).
    Records (checksum, checksum cycle, frame cycle, frame hash) after every step, then get_audio over the script's
    range. Returns (records, (start_cycle, end_cycle, PCM16 base64))."""
    records = []

    def record():
        cs = op.call("get_state_checksum")
        frame = agent.call("get_frame", format="BGRA32")
        records.append((cs["sha256"], cs["emulated_cycles"], frame["emulated_cycles"], frame_hash(frame)))

    record()
    agent.call("type_text", text=PRINT_INPUT_STATE, inter_key_delay_ms=0)
    op.call("step_frames", frames=30)
    record()
    agent.call("type_text", text='10 PRINT "HI"\n', inter_key_delay_ms=0)
    op.call("step_frames", frames=40)
    record()
    op.call("step_cycles", cycles=12345)
    record()
    agent.call("type_text", text="PRINT CHR$(7)\n", inter_key_delay_ms=20)
    op.call("step_cycles", cycles=7 * CYCLES_PER_FRAME + 3)
    record()
    op.call("step_frames", frames=60)
    record()
    agent.call("press_key", key="A", hold_ms=50)
    op.call("step_frames", frames=30)
    record()
    agent.call("type_text", text="CATALOG\n", inter_key_delay_ms=0)
    op.call("step_frames", frames=90)
    record()
    end = op.call("step_cycles", cycles=1)["emulated_cycles"]
    record()
    duration_ms = int((end - from_cycle) * 1000.0 / BASE_CLOCK_HZ) + 2
    audio = agent.call("get_audio", from_cycle=from_cycle, duration_ms=duration_ms, format="PCM16")
    return records, (audio["start_cycle"], audio["end_cycle"], audio["data"])


def unrelated_work(op, agent):
    """Leaves the machine and the harness busy AT the restore (review I2): BUSY_PROGRAM has the Disk II motor on, the
    head stepped out to track 0 (run 1's CATALOG leaves it on track 17) and the speaker toggling in a loop that is still
    running; joystick 0 moved with button 0 down, a key held, text queued behind it, and an audio subscription
    mid-sound."""
    agent.call("subscribe_audio")
    agent.call("type_text", text=BUSY_PROGRAM, inter_key_delay_ms=0)
    op.call("step_frames", frames=240)
    agent.call("set_joystick", x=0, y=255, buttons={"0": True}, duration_ms=5000)
    agent.call("press_key", key="Z", hold_ms=5000)
    agent.call("type_text", text="LIST\n", inter_key_delay_ms=500)
    op.call("step_frames", frames=20)
    op.call("step_cycles", cycles=4321)


class CheckpointTestCase(PipeTestCase):
    def launch(self, headless=True, timeout_s=90.0):
        """As PipeTestCase.launch, with a longer watchdog (checkpoint tests boot DOS and replay scripts)."""
        harness = Harness.launch(headless=headless)
        self.addCleanup(harness.close)
        watchdog = threading.Timer(timeout_s, harness.kill)
        watchdog.daemon = True
        watchdog.start()
        self.addCleanup(watchdog.cancel)
        return harness

    def booted(self, harness, disks=None):
        """A stepped session that has booted DOS 3.3 (720 frames) from disks (default: the System Master), and an agent
        connection."""
        op = self.stepped_session(harness, disks=dos_disks() if disks is None else disks)
        op.call("step_frames", frames=720, timeout=60.0)
        return op, harness.connect("agent")

    def scratch_dir(self):
        """A directory outside the harness work directory, removed on cleanup."""
        path = tempfile.mkdtemp(prefix="awh-m4-")
        self.addCleanup(shutil.rmtree, path, True)
        return path

    def replay(self, headless=True, speeds=None):
        """Pass condition 1 (and 2 with speeds=(a, b)), ruling D4: checkpoint, some frames, restore, script; unrelated
        work, restore, script. Returns both runs, each started from a restore."""
        h = self.launch(headless=headless)
        op, agent = self.booted(h)
        if speeds:
            op.call("set_speed", machine_speed=speeds[0])
        at = op.call("step_frames", frames=1)["emulated_cycles"]
        cp = op.call("checkpoint", name="replay")
        self.assertEqual(sorted(cp), ["checkpoint_id", "emulated_cycles"])
        self.assertEqual(cp["emulated_cycles"], at)
        at_checksum = op.call("get_state_checksum")
        self.assertEqual(at_checksum["emulated_cycles"], at)
        op.call("step_frames", frames=30)   # anything
        self.assertEqual(op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"]), {"emulated_cycles": at})
        self.assertEqual(op.call("get_state_checksum"), at_checksum)
        first = run_script(op, agent, at)
        self.assertGreater(mono_ac_rms_db(base64.b64decode(first[1][2])), -50.0,
                           "the script's bell left no audio: the audio comparison would prove nothing")

        unrelated_work(op, agent)
        self.assertEqual(op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"]), {"emulated_cycles": at})
        self.assertEqual(op.call("get_state_checksum"), at_checksum)
        if speeds:
            op.call("set_speed", machine_speed=speeds[1])
            self.assertEqual(op.call("get_speed")["machine_speed"], speeds[1])
        second = run_script(op, agent, at)
        return first, second

    def assertSameRuns(self, a, b):
        records_a, audio_a = a
        records_b, audio_b = b
        self.assertEqual(len(records_a), len(records_b))
        for i, (ra, rb) in enumerate(zip(records_a, records_b)):
            self.assertEqual(ra, rb, "step {} differs".format(i))
        self.assertEqual(audio_a[:2], audio_b[:2], "get_audio ranges differ")
        self.assertTrue(audio_a[2] == audio_b[2], "get_audio bytes differ")


# ---- Pass conditions 1 and 2 ----

class ReplayTest(CheckpointTestCase):
    def test_restored_run_replays_the_script_exactly(self):
        """Pass condition 1 and Review Focus 2: two restores from very different prior states replay identically."""
        first, second = self.replay()
        self.assertSameRuns(first, second)

    def test_replay_is_the_same_at_machine_speed_10_and_40(self):
        """Pass condition 2."""
        first, second = self.replay(speeds=(10, 40))
        self.assertSameRuns(first, second)


# ---- Pass condition 4 and Review Focus 5 ----

class DiskTest(CheckpointTestCase):
    LOAD_ROWS = range(0, 3)   # "]LOAD HELLO2", "]LIST", and the row after: never the cursor's row

    def load_and_list(self, op, agent):
        """HOME, LOAD HELLO2, LIST; the BGRA32 pixels of the top text rows. After a successful LOAD row 1 is "]LIST";
        after FILE NOT FOUND it is not."""
        agent.call("type_text", text="HOME\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=10)
        agent.call("type_text", text="LOAD HELLO2\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=300)
        agent.call("type_text", text="LIST\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=60)
        frame = agent.call("get_frame", format="BGRA32")
        return b"".join(text_row_band(frame, row) for row in self.LOAD_ROWS)

    def rows_after_fresh_boot(self, image):
        h = self.launch()
        op, agent = self.booted(h, disks=[{"slot": 6, "drive": 1, "path": image}])
        rows = self.load_and_list(op, agent)
        h.close()
        return rows

    def test_save_export_and_restore(self):
        """Pass condition 4."""
        sha_before = sha256_file(DOS_IMAGE)
        mtime_before = os.stat(DOS_IMAGE).st_mtime_ns
        out = self.scratch_dir()

        h = self.launch(timeout_s=150.0)
        op, agent = self.booted(h)
        disks = op.call("list_disks")
        self.assertEqual(len(disks), 1, disks)
        d = disks[0]
        self.assertEqual(sorted(d), ["drive", "format", "original_path", "slot", "working_path", "write_protected"])
        self.assertEqual((d["slot"], d["drive"], d["write_protected"], d["format"]), (6, 1, False, "DO"))
        self.assertTrue(same_path(d["original_path"], DOS_IMAGE), d)
        self.assertTrue(os.path.normcase(os.path.realpath(d["working_path"])).startswith(
            os.path.normcase(os.path.realpath(work_dir(h))) + os.sep), d)

        cp = op.call("checkpoint", name="before SAVE")
        before_save = os.path.join(out, "before.dsk")
        self.assertEqual(op.call("export_disk", slot=6, drive=1, dest_path=before_save), {})

        agent.call("type_text", text='10 PRINT "HI"\n', inter_key_delay_ms=0)
        op.call("step_frames", frames=30)
        agent.call("type_text", text="SAVE HELLO2\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=300)
        after_save = os.path.join(out, "after.dsk")
        self.assertEqual(op.call("export_disk", slot=6, drive=1, dest_path=after_save), {})
        self.assertNotEqual(sha256_file(after_save), sha256_file(before_save), "SAVE did not reach the export")
        reference_rows = self.load_and_list(op, agent)   # LOAD from the session's own disk
        self.assertEqual((sha256_file(DOS_IMAGE), os.stat(DOS_IMAGE).st_mtime_ns), (sha_before, mtime_before))

        self.assertEqual(op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"]),
                         {"emulated_cycles": cp["emulated_cycles"]})
        restored = os.path.join(out, "restored.dsk")
        self.assertEqual(op.call("export_disk", slot=6, drive=1, dest_path=restored), {})
        self.assertEqual(sha256_file(restored), sha256_file(before_save), "the restore did not bring the disk back")
        h.close()

        # A fresh boot from the export finds HELLO2; a fresh boot from the original does not (so the check can fail).
        self.assertEqual(self.rows_after_fresh_boot(after_save), reference_rows, "HELLO2 is not on the exported disk")
        self.assertNotEqual(self.rows_after_fresh_boot(DOS_IMAGE), reference_rows,
                            "LOAD HELLO2 looks the same without the file: the check proves nothing")
        self.assertEqual((sha256_file(DOS_IMAGE), os.stat(DOS_IMAGE).st_mtime_ns), (sha_before, mtime_before))

    def test_export_disk_refuses_unsafe_destinations(self):
        """Review Focus 5."""
        sha_before = sha256_file(DOS_IMAGE)
        mtime_before = os.stat(DOS_IMAGE).st_mtime_ns
        out = self.scratch_dir()
        existing = os.path.join(out, "existing.dsk")
        with open(existing, "wb") as f:
            f.write(b"keep me")
        h = self.launch()
        op = self.stepped_session(h, disks=dos_disks())
        op.call("step_frames", frames=10)
        inside_work = os.path.join(work_dir(h), "export.dsk")
        for dest in ("export.dsk", os.path.join("sub", "export.dsk"), existing, DOS_IMAGE, inside_work):
            with self.subTest(dest=dest):
                self.assertRpcError(-32602, op, "export_disk", slot=6, drive=1, dest_path=dest)
        self.assertFalse(os.path.exists(inside_work))
        with open(existing, "rb") as f:
            self.assertEqual(f.read(), b"keep me")
        self.assertRpcError(-32602, op, "export_disk", slot=5, drive=1, dest_path=os.path.join(out, "a.dsk"))
        self.assertRpcError(-32602, op, "export_disk", slot=6, drive=3, dest_path=os.path.join(out, "a.dsk"))
        self.assertRpcError(-32602, op, "export_disk", slot=6, drive=1)
        self.assertRpcError(-32005, op, "export_disk", slot=6, drive=2, dest_path=os.path.join(out, "empty.dsk"))
        self.assertFalse(os.path.exists(os.path.join(out, "empty.dsk")))
        self.assertEqual((sha256_file(DOS_IMAGE), os.stat(DOS_IMAGE).st_mtime_ns), (sha_before, mtime_before))

    def test_list_disks_leaves_out_empty_drives(self):
        h = self.launch()
        op = self.stepped_session(h)
        self.assertEqual(op.call("list_disks"), [])


# ---- Review Focus 4 ----

class RunningCheckpointTest(CheckpointTestCase):
    def test_checkpoint_while_running_at_max_ends_the_realtime_run(self):
        """Ruling D5: a checkpoint taken while Running at "max" carries no full speed or Mockingboard mute; two restores
        (with unrelated work between) give the same state, and full speed is never active after a restore."""
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", run_mode="realtime", disks=dos_disks(), speed={"machine_speed": "max"})
        time.sleep(0.5)
        cp = op.call("checkpoint")
        self.assertFalse(op.call("get_speed")["full_speed_active"])
        agent = h.connect("agent")

        self.assertEqual(op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"]),
                         {"emulated_cycles": cp["emulated_cycles"]})
        self.assertFalse(op.call("get_speed")["full_speed_active"])
        first = op.call("get_state_checksum")
        self.assertEqual(first["emulated_cycles"], cp["emulated_cycles"])

        unrelated_work(op, agent)   # stepped while Paused: keys, a bell, the disk head, frames
        self.assertEqual(op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"]),
                         {"emulated_cycles": cp["emulated_cycles"]})
        self.assertFalse(op.call("get_speed")["full_speed_active"])
        self.assertEqual(op.call("get_state_checksum"), first)


# ---- Restore purity in the harness ----

class RestorePurityTest(CheckpointTestCase):
    def test_input_pending_at_restore_is_reported_discarded(self):
        """Ruling D1."""
        h = self.launch()
        op = self.stepped_session(h)   # no disk: the boot ROM never reads the keyboard, so typed keys stay queued
        op.call("step_frames", frames=10)
        cp = op.call("checkpoint")
        agent = h.connect("agent")
        ids = [agent.call("type_text", text="ABC", inter_key_delay_ms=0)["input_id"],
               agent.call("press_key", key="Q", hold_ms=100)["input_id"],
               agent.call("set_joystick", x=5, duration_ms=1000)["input_id"]]
        op.call("step_frames", frames=1)
        self.assertEqual([n for n in agent.drain_notifications(quiet=0.3) if n.get("method") == "input_applied"], [])

        op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        notes = [n for n in agent.drain_notifications(quiet=0.5) if n.get("method") == "input_applied"]
        self.assertEqual(sorted((n["params"] for n in notes), key=lambda p: p["input_id"]),
                         [{"input_id": i, "first_cycle": None, "last_cycle": None, "discarded": True} for i in ids])
        op.call("step_frames", frames=120)
        self.assertEqual([n for n in agent.drain_notifications(quiet=0.3) if n.get("method") == "input_applied"], [])

    def test_sound_detectors_reset_to_the_restored_cycle(self):
        """Restore purity: a subscription that is mid-sound at the restore reports nothing more about that sound, and
        its next events describe only audio after the restore."""
        h = self.launch()
        op, agent = self.booted(h)
        op.call("step_frames", frames=30)
        cp = op.call("checkpoint")
        at = cp["emulated_cycles"]
        self.assertEqual(agent.call("subscribe_audio"), {})
        agent.call("type_text", text=BUSY_PROGRAM, inter_key_delay_ms=0)
        op.call("step_frames", frames=300)
        before = [n for n in agent.drain_notifications(quiet=0.5) if n.get("method") in SOUND_EVENTS]
        self.assertTrue(before and before[-1]["method"] == "sound_started", "the tone is not sounding: {}".format(before))
        tone_start = before[-1]["params"]["start_cycle"]

        self.assertEqual(op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"]), {"emulated_cycles": at})
        agent.call("type_text", text="PRINT CHR$(7)\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=120)
        after = [n for n in agent.drain_notifications(quiet=0.5) if n.get("method") in SOUND_EVENTS]
        self.assertEqual([n["method"] for n in after], ["sound_started", "sound_stopped"], after)
        started, stopped = after[0]["params"], after[1]["params"]
        self.assertEqual(stopped["start_cycle"], started["start_cycle"])
        self.assertGreaterEqual(started["start_cycle"], at)
        self.assertNotEqual(started["start_cycle"], tone_start, "an event about the pre-restore tone")
        self.assertLess(stopped["duration_ms"], 300, "the bell's event spans pre-restore audio: {}".format(stopped))

    def test_audio_from_before_the_restore_is_gone(self):
        h = self.launch()
        op, agent = self.booted(h)
        agent.call("type_text", text="PRINT CHR$(7)\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=60)
        cp = op.call("checkpoint")
        at = cp["emulated_cycles"]
        agent.call("type_text", text="PRINT CHR$(7)\n", inter_key_delay_ms=0)
        op.call("step_frames", frames=120)
        self.assertGreater(len(base64.b64decode(agent.call("get_audio", from_cycle=at - ms_to_cycles(500),
                                                           duration_ms=100, format="PCM16")["data"])), 0)

        op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        self.assertRpcError(-32009, agent, "get_audio", from_cycle=at - ms_to_cycles(500), duration_ms=100)
        self.assertRpcError(-32009, agent, "get_audio", from_cycle=0, duration_ms=100)
        after = agent.call("get_audio", from_cycle=at, duration_ms=1000, format="PCM16")
        self.assertEqual(base64.b64decode(after["data"]), b"", "audio of the abandoned run is still there")


# ---- API shapes, limits and errors ----

class CheckpointApiTest(CheckpointTestCase):
    def test_list_and_delete_checkpoints(self):
        """The list_/delete_checkpoint shapes, and Review Focus 1: restoring after deleting a different checkpoint."""
        h = self.launch()
        op = self.stepped_session(h)
        op.call("step_frames", frames=10)
        a = op.call("checkpoint", name="first")
        self.assertEqual(a["checkpoint_id"], "cp-1")
        op.call("step_frames", frames=5)
        b = op.call("checkpoint")
        self.assertEqual(b["checkpoint_id"], "cp-2")
        self.assertGreater(b["emulated_cycles"], a["emulated_cycles"])

        listed = op.call("list_checkpoints")
        self.assertEqual(len(listed), 2)
        for entry in listed:
            self.assertEqual(sorted(entry), ["checkpoint_id", "created_at", "emulated_cycles", "name"])
            self.assertRegex(entry["created_at"], ISO_UTC)
        self.assertEqual([(e["checkpoint_id"], e["name"], e["emulated_cycles"]) for e in listed],
                         [("cp-1", "first", a["emulated_cycles"]), ("cp-2", None, b["emulated_cycles"])])
        self.assertLessEqual(listed[0]["created_at"], listed[1]["created_at"])

        self.assertEqual(op.call("delete_checkpoint", checkpoint_id="cp-1"), {})
        self.assertEqual([e["checkpoint_id"] for e in op.call("list_checkpoints")], ["cp-2"])
        self.assertFalse(os.path.exists(os.path.join(work_dir(h), "checkpoints", "cp-1")))
        self.assertEqual(op.call("restore_checkpoint", checkpoint_id="cp-2"), {"emulated_cycles": b["emulated_cycles"]})

        self.assertRpcError(-32602, op, "restore_checkpoint", checkpoint_id="cp-1")
        self.assertRpcError(-32602, op, "delete_checkpoint", checkpoint_id="cp-1")
        self.assertRpcError(-32602, op, "restore_checkpoint", checkpoint_id="nope")
        self.assertRpcError(-32602, op, "restore_checkpoint")
        self.assertRpcError(-32602, op, "delete_checkpoint")
        self.assertEqual(op.call("checkpoint")["checkpoint_id"], "cp-3")   # ids are never reused

        self.assertEqual(op.call("checkpoint", name="x" * 64)["checkpoint_id"], "cp-4")
        for bad in ("x" * 65, "tab\there", "nul\x00", 5, ["a"]):
            with self.subTest(name=repr(bad)):
                self.assertRpcError(-32602, op, "checkpoint", name=bad)
        self.assertEqual(op.call("checkpoint")["checkpoint_id"], "cp-5")   # refused names use up no id
        self.assertTrue(os.path.isfile(os.path.join(work_dir(h), "checkpoints", "cp-5", "state.yaml")))
        self.assertTrue(os.path.isfile(os.path.join(work_dir(h), "checkpoints", "cp-5", "meta.json")))

    def test_256_checkpoints_then_32004(self):
        h = self.launch(timeout_s=240.0)
        op = self.stepped_session(h)   # no disks: each checkpoint is just the save state
        for n in range(1, 257):
            self.assertEqual(op.call("checkpoint")["checkpoint_id"], "cp-{}".format(n))
        self.assertRpcError(-32004, op, "checkpoint")
        self.assertEqual(len(op.call("list_checkpoints")), 256)
        op.call("delete_checkpoint", checkpoint_id="cp-1")
        self.assertEqual(op.call("checkpoint")["checkpoint_id"], "cp-257")

    def test_state_checksum_and_step_cycles(self):
        h = self.launch()
        op = self.stepped_session(h)
        start = op.call("step_frames", frames=10)["emulated_cycles"]
        cs = op.call("get_state_checksum")
        self.assertEqual(sorted(cs), ["emulated_cycles", "sha256"])
        self.assertRegex(cs["sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(cs["emulated_cycles"], start)
        self.assertEqual(op.call("get_state_checksum"), cs)

        stepped = op.call("step_cycles", cycles=12345)
        self.assertEqual(sorted(stepped), ["emulated_cycles"])
        self.assertGreaterEqual(stepped["emulated_cycles"] - start, 12345)
        self.assertLessEqual(stepped["emulated_cycles"] - start, 12345 + 7)
        self.assertNotEqual(op.call("get_state_checksum")["sha256"], cs["sha256"])
        for bad in (0, MAX_STEP_CYCLES + 1, -1, "100", None):
            with self.subTest(cycles=bad):
                self.assertRpcError(-32602, op, "step_cycles", cycles=bad)
        self.assertRpcError(-32602, op, "step_cycles")
        before = op.call("get_state_checksum")["emulated_cycles"]
        biggest = op.call("step_cycles", cycles=MAX_STEP_CYCLES, timeout=60.0)["emulated_cycles"]
        self.assertGreaterEqual(biggest - before, MAX_STEP_CYCLES)
        self.assertLessEqual(biggest - before, MAX_STEP_CYCLES + 7)

    def test_step_cycles_pauses_a_running_session(self):
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", run_mode="realtime")
        time.sleep(0.3)
        stepped = op.call("step_cycles", cycles=1000)["emulated_cycles"]
        time.sleep(0.3)
        self.assertEqual(op.call("get_state_checksum")["emulated_cycles"], stepped, "still running after step_cycles")

    def test_restore_leaves_a_realtime_session_paused(self):
        h = self.launch()
        op = h.connect("operator")
        op.call("create_session", run_mode="realtime")
        time.sleep(0.3)
        cp = op.call("checkpoint")
        op.call("resume")
        time.sleep(0.3)
        op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        time.sleep(0.3)
        self.assertEqual(op.call("get_state_checksum")["emulated_cycles"], cp["emulated_cycles"])

    def test_new_methods_are_operator_only(self):
        h = self.launch()
        self.stepped_session(h)
        agent = h.connect("agent")
        for method, params in (("checkpoint", {}), ("restore_checkpoint", {"checkpoint_id": "cp-1"}),
                               ("list_checkpoints", {}), ("delete_checkpoint", {"checkpoint_id": "cp-1"}),
                               ("get_state_checksum", {}), ("step_cycles", {"cycles": 1}), ("list_disks", {}),
                               ("export_disk", {"slot": 6, "drive": 1, "dest_path": "C:\\x.dsk"})):
            with self.subTest(method=method):
                self.assertRpcError(-32001, agent, method, **params)

    def test_new_methods_before_a_session_get_32002(self):
        h = self.launch()
        op = h.connect("operator")
        for method, params in (("checkpoint", {}), ("restore_checkpoint", {"checkpoint_id": "cp-1"}),
                               ("list_checkpoints", {}), ("delete_checkpoint", {"checkpoint_id": "cp-1"}),
                               ("get_state_checksum", {}), ("step_cycles", {"cycles": 1}), ("list_disks", {}),
                               ("export_disk", {"slot": 6, "drive": 1, "dest_path": "C:\\x.dsk"})):
            with self.subTest(method=method):
                self.assertRpcError(-32002, op, method, **params)


# ---- Restore failures (Review Focus 1 and 3) ----

class RestoreFailureTest(CheckpointTestCase):
    def assertUnusable(self, op):
        self.assertRpcError(-32008, op, "step_frames", frames=1)
        self.assertRpcError(-32008, op, "step_cycles", cycles=1)

    def test_restore_with_a_disk_copy_missing_gets_32004_and_stays_usable(self):
        h = self.launch()
        op, _ = self.booted(h)
        cp = op.call("checkpoint")
        cp_dir = os.path.join(work_dir(h), "checkpoints", cp["checkpoint_id"])
        disk_copies = [f for f in os.listdir(cp_dir) if f not in ("state.yaml", "meta.json")]
        self.assertTrue(disk_copies, "no disk copy in {}".format(cp_dir))
        for f in disk_copies:
            os.remove(os.path.join(cp_dir, f))
        op.call("step_frames", frames=30)
        before = op.call("get_state_checksum")
        disks_before = op.call("list_disks")
        self.assertRpcError(-32004, op, "restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        # Ruling D7: the failure came before Snapshot_LoadState, so the session stays usable and nothing changed.
        self.assertEqual(op.call("get_state_checksum"), before)
        self.assertEqual(op.call("list_disks"), disks_before)
        stepped = op.call("step_frames", frames=1)["emulated_cycles"]
        self.assertGreater(stepped, before["emulated_cycles"])

    def test_restore_failing_after_the_first_disk_is_copied_rolls_back(self):
        """Ruling D7 (review fix round 1): the second checkpoint disk copy cannot be read, so the restore fails after
        the first working disk was already overwritten. The working disks, drives and machine state are as before."""
        import ctypes
        h = self.launch()
        disks = [{"slot": 6, "drive": 1, "path": DOS_IMAGE}, {"slot": 6, "drive": 2, "path": DOS_IMAGE}]
        op, _ = self.booted(h, disks=disks)
        cp = op.call("checkpoint")
        cp_dir = os.path.join(work_dir(h), "checkpoints", cp["checkpoint_id"])
        copy1 = [f for f in os.listdir(cp_dir) if f.startswith("disk1-")]
        copy2 = [f for f in os.listdir(cp_dir) if f.startswith("disk2-")]
        self.assertEqual((len(copy1), len(copy2)), (1, 1), os.listdir(cp_dir))
        with open(os.path.join(cp_dir, copy1[0]), "r+b") as f:   # drive 1's copy differs from its working disk
            f.seek(0x1000)
            b = f.read(1)
            f.seek(0x1000)
            f.write(bytes([b[0] ^ 0xFF]))
        op.call("step_frames", frames=30)
        before = op.call("get_state_checksum")
        disks_before = op.call("list_disks")
        hashes_before = [sha256_file(d["working_path"]) for d in disks_before]
        self.assertEqual(len(hashes_before), 2)

        kernel32 = ctypes.windll.kernel32
        kernel32.CreateFileW.restype = ctypes.c_void_p
        kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                         ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        lock = kernel32.CreateFileW(os.path.join(cp_dir, copy2[0]), 0x80000000, 0, None, 3, 0, None)
        self.assertNotIn(lock, (None, ctypes.c_void_p(-1).value), "could not lock the second disk copy")
        try:
            self.assertRpcError(-32004, op, "restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        finally:
            kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
            kernel32.CloseHandle(lock)
        self.assertEqual(op.call("get_state_checksum"), before)
        disks_after = op.call("list_disks")
        self.assertEqual(disks_after, disks_before)
        self.assertEqual([sha256_file(d["working_path"]) for d in disks_after], hashes_before)
        for d in disks_after:
            self.assertFalse(os.path.exists(d["working_path"] + ".restore-bak"))
        self.assertGreater(op.call("step_frames", frames=1)["emulated_cycles"], before["emulated_cycles"])

    def test_restore_of_a_corrupt_state_file_gets_32004_with_the_message(self):
        h = self.launch()
        op, _ = self.booted(h)
        cp = op.call("checkpoint")
        state = os.path.join(work_dir(h), "checkpoints", cp["checkpoint_id"], "state.yaml")
        with open(state, "rb") as f:
            data = f.read()
        self.assertGreater(len(data), 1000)
        with open(state, "wb") as f:   # the header survives, so the load starts replacing state before it fails
            f.write(data[:len(data) * 2 // 5] + b"\n  ]]] not: [yaml\n")
        rid = op.request("restore_checkpoint", {"checkpoint_id": cp["checkpoint_id"]})
        reply = op.wait_reply(rid, timeout=30.0)
        self.assertIn("error", reply, reply)
        self.assertEqual(reply["error"]["code"], -32004, reply)
        self.assertTrue(reply["error"]["message"].strip(), "the -32004 carries no message")
        self.assertUnusable(op)


# ---- Pass condition 5 (speech; the program is M4 Task 4's) ----

class SpeechTest(CheckpointTestCase):
    def test_speech_replays_identically_across_two_restores(self):
        if SSI263_PROGRAM is None:
            self.fail(SSI263_MISSING)


# ---- Windowed: pass conditions 3 and 5 ----

@unittest.skipUnless(WINDOWED, "set HARNESS_WINDOWED=1 to run windowed tests")
class WindowedCheckpointTest(CheckpointTestCase):
    def test_windowed_replay_matches_headless(self):
        """Pass condition 3."""
        headless = self.replay(headless=True)
        windowed = self.replay(headless=False)
        self.assertSameRuns(headless[0], headless[1])
        self.assertSameRuns(windowed[0], windowed[1])
        self.assertSameRuns(headless[0], windowed[0])

    def test_speech_is_the_same_in_both_display_modes(self):
        """Pass condition 5."""
        if SSI263_PROGRAM is None:
            self.fail(SSI263_MISSING)


if __name__ == "__main__":
    unittest.main()
