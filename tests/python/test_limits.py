# SPDX-License-Identifier: MIT
"""Input and request limits over the pipe (spec: docs/specs/m6.md "Input and request limits"; pass conditions 1-3;
plan Review Focus 1 and 3).

Run: python -m unittest discover -s tests/python -v   (HARNESS_EXE selects the exe; default bin/x64/Debug).
"""

import json
import os
import struct
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "adapter"))   # applewin_mcp.harness_client

from applewin_mcp.harness_client import RpcError
from test_pipe import DOS_IMAGE, PipeTestCase

QUEUE_LIMIT = 4096
MAX_REQUEST = 1024 * 1024
BOOT_FRAMES = 720
INVALID_PARAMS = -32602
QUEUE_FULL = -32010


def utf16_units(s):
    return len(s.encode("utf-16-le")) // 2


class LimitTestCase(PipeTestCase):
    def stepped(self, harness, boot=False):
        """(operator, agent) with a stepped session. boot=True runs the DOS 3.3 master to its prompt first, so that
        queued keys are consumed (a machine that is still looking for a boot disk never reads the keyboard)."""
        op = harness.connect("operator")
        params = {"run_mode": "stepped"}
        if boot:
            params["disks"] = [{"slot": 6, "drive": 1, "path": DOS_IMAGE}]
        op.call("create_session", **params)
        agent = harness.connect("agent")
        if boot:
            op.call("step_frames", frames=BOOT_FRAMES, timeout=60.0)
        return op, agent


# ---- Pass condition 1: per-value limits ----

class ValueLimitTest(LimitTestCase):
    def test_hold_ms_range(self):
        h = self.launch()
        _, agent = self.stepped(h)
        for ok in (0, 1, 10000):
            with self.subTest(hold_ms=ok):
                self.assertIn("input_id", agent.call("press_key", key="A", hold_ms=ok))
        for bad in (10001, 1000000, -1):
            with self.subTest(hold_ms=bad):
                self.assertRpcError(INVALID_PARAMS, agent, "press_key", key="A", hold_ms=bad)

    def test_duration_ms_range(self):
        h = self.launch()
        _, agent = self.stepped(h)
        for ok in (0, 60000):
            with self.subTest(duration_ms=ok):
                self.assertIn("input_id", agent.call("set_joystick", x=100, duration_ms=ok))
        for bad in (60001, 3600000, -1):
            with self.subTest(duration_ms=bad):
                self.assertRpcError(INVALID_PARAMS, agent, "set_joystick", x=100, duration_ms=bad)

    def test_inter_key_delay_ms_range(self):
        h = self.launch()
        _, agent = self.stepped(h)
        for ok in (0, 1000):
            with self.subTest(inter_key_delay_ms=ok):
                self.assertIn("input_id", agent.call("type_text", text="AB", inter_key_delay_ms=ok))
        for bad in (1001, 100000, -1):
            with self.subTest(inter_key_delay_ms=bad):
                self.assertRpcError(INVALID_PARAMS, agent, "type_text", text="AB", inter_key_delay_ms=bad)

    def test_text_length(self):
        h = self.launch()
        _, agent = self.stepped(h)
        self.assertIn("input_id", agent.call("type_text", text="A" * QUEUE_LIMIT, inter_key_delay_ms=0))
        self.assertRpcError(INVALID_PARAMS, agent, "type_text", text="A" * (QUEUE_LIMIT + 1), inter_key_delay_ms=0)

    def test_a_rejected_request_queues_nothing(self):
        h = self.launch()
        _, agent = self.stepped(h)
        self.assertRpcError(INVALID_PARAMS, agent, "press_key", key="A", hold_ms=10001)
        self.assertRpcError(INVALID_PARAMS, agent, "type_text", text="AB", inter_key_delay_ms=1001)
        self.assertRpcError(INVALID_PARAMS, agent, "set_joystick", x=1, duration_ms=60001)
        # all of the queue is still free: a text of exactly the cap is accepted
        self.assertIn("input_id", agent.call("type_text", text="A" * QUEUE_LIMIT, inter_key_delay_ms=0))

    def test_string_parameter_length(self):
        """Any string parameter: 32,767 UTF-16 code units. An absolute path of that length passes the limit (and then
        fails as a missing file, -32005); one unit more is -32602 before the file is looked at."""
        h = self.launch()
        op, _ = self.stepped(h)
        prefix = "C:\\"
        at_limit = prefix + "a" * (32767 - len(prefix))
        self.assertEqual(utf16_units(at_limit), 32767)
        with self.subTest(case="ascii at the limit"):
            self.assertRpcError(-32005, op, "insert_disk", slot=6, drive=2, path=at_limit)
        with self.subTest(case="ascii over the limit"):
            self.assertRpcError(INVALID_PARAMS, op, "insert_disk", slot=6, drive=2, path=at_limit + "a")
        astral = "\U0001F600"   # two UTF-16 code units
        at_limit = prefix + astral * ((32767 - len(prefix)) // 2)   # 3 + 2 * 16382 = 32767
        self.assertEqual(utf16_units(at_limit), 32767)
        with self.subTest(case="surrogate pairs at the limit"):
            self.assertRpcError(-32005, op, "insert_disk", slot=6, drive=2, path=at_limit)
        with self.subTest(case="surrogate pairs over the limit"):
            self.assertRpcError(INVALID_PARAMS, op, "insert_disk", slot=6, drive=2, path=at_limit + "a")
        with self.subTest(case="export_disk dest_path over the limit"):
            self.assertRpcError(INVALID_PARAMS, op, "export_disk", slot=6, drive=1, dest_path=prefix + "a" * 32768)

    def test_new_methods_are_operator_only(self):
        h = self.launch()
        _, agent = self.stepped(h)
        self.assertRpcError(-32001, agent, "subscribe_disk_writes")
        self.assertRpcError(-32001, agent, "unsubscribe_disk_writes")


# ---- Pass condition 2 / Review Focus 1: the queue limit ----

class QueueLimitTest(LimitTestCase):
    def assertQueueFull(self, agent, pending, method, **params):
        with self.assertRaises(RpcError) as ctx:
            agent.call(method, **params)
        self.assertEqual(ctx.exception.code, QUEUE_FULL, str(ctx.exception))
        self.assertEqual(ctx.exception.data, {"pending": pending, "limit": QUEUE_LIMIT})

    def drain(self, op, agent, input_ids):
        """Steps until input_applied has arrived for every id."""
        waiting = set(input_ids)
        stepped = 0
        seen = 0
        for _ in range(160):   # 48,000 frames: twice the 24,000 estimated for 4,096 keys; exits early on success
            op.call("step_frames", frames=300, timeout=60.0)
            stepped += 300
            for note in agent.drain_notifications(quiet=0.05, limit=2.0):
                if note.get("method") == "input_applied":
                    seen += 1
                    waiting.discard(note["params"]["input_id"])
            if not waiting:
                return
        self.fail("inputs {} were never applied after {} frames ({} input_applied notifications seen; input_applied "
                  "covers a whole type_text, so none of its up to {} keys is reported as applied before the last)"
                  .format(sorted(waiting), stepped, seen, QUEUE_LIMIT))

    def test_a_full_queue_rejects_one_more_key_and_drains_normally(self):
        h = self.launch()
        op, agent = self.stepped(h, boot=True)
        first = agent.call("type_text", text=" " * QUEUE_LIMIT, inter_key_delay_ms=0)   # exactly the cap
        self.assertQueueFull(agent, QUEUE_LIMIT, "press_key", key="A", hold_ms=0)
        self.assertQueueFull(agent, QUEUE_LIMIT, "type_text", text="A", inter_key_delay_ms=0)
        self.assertQueueFull(agent, QUEUE_LIMIT, "set_joystick", x=1)
        self.drain(op, agent, [first["input_id"]])
        # the session is usable and the queue is empty again
        again = agent.call("type_text", text=" " * QUEUE_LIMIT, inter_key_delay_ms=0)
        self.assertIn("input_id", again)
        self.assertQueueFull(agent, QUEUE_LIMIT, "press_key", key="A", hold_ms=0)

    def test_the_check_is_all_or_nothing(self):
        """Review Focus 1: with 4,090 pending, a 10-character type_text is rejected whole. Nothing from it is queued,
        so 6 more events still fit (4,096), and one more is rejected."""
        h = self.launch()
        _, agent = self.stepped(h)
        agent.call("type_text", text=" " * 4090, inter_key_delay_ms=0)
        self.assertQueueFull(agent, 4090, "type_text", text="0123456789", inter_key_delay_ms=0)
        agent.call("type_text", text=" " * 6, inter_key_delay_ms=0)   # fits only if the rejected 10 queued nothing
        self.assertQueueFull(agent, QUEUE_LIMIT, "press_key", key="A", hold_ms=0)

    def test_a_timed_joystick_change_counts_as_two_events(self):
        h = self.launch()
        _, agent = self.stepped(h)
        agent.call("type_text", text=" " * 4095, inter_key_delay_ms=0)
        self.assertQueueFull(agent, 4095, "set_joystick", x=1, duration_ms=100)   # the change and its release: 2
        self.assertIn("input_id", agent.call("set_joystick", x=1))                # a plain change: 1
        self.assertQueueFull(agent, QUEUE_LIMIT, "set_joystick", x=2)

    def test_restore_checkpoint_empties_the_queue(self):
        h = self.launch()
        op, agent = self.stepped(h, boot=True)
        cp = op.call("checkpoint", name="before")
        agent.call("type_text", text=" " * QUEUE_LIMIT, inter_key_delay_ms=0)
        self.assertQueueFull(agent, QUEUE_LIMIT, "press_key", key="A", hold_ms=0)
        op.call("restore_checkpoint", checkpoint_id=cp["checkpoint_id"])
        self.assertIn("input_id", agent.call("type_text", text=" " * QUEUE_LIMIT, inter_key_delay_ms=0))

    def test_limit_rejections_are_logged_to_operators_not_the_agent(self):
        h = self.launch()
        op, agent = self.stepped(h)
        agent.call("type_text", text=" " * QUEUE_LIMIT, inter_key_delay_ms=0)
        self.assertQueueFull(agent, QUEUE_LIMIT, "press_key", key="A", hold_ms=0)
        notes = [n for n in op.drain_notifications(quiet=0.5, limit=5.0) if n.get("method") == "log"]
        self.assertEqual([n["params"]["level"] for n in notes], ["warning"])
        self.assertEqual([n for n in agent.drain_notifications(quiet=0.3, limit=2.0) if n.get("method") == "log"], [])


# ---- Pass condition 3: request size ----

class RequestSizeTest(LimitTestCase):
    def padded_request(self, request_id, size):
        base = {"jsonrpc": "2.0", "id": request_id, "method": "press_key", "params": {"key": "A", "hold_ms": 0}, "pad": ""}
        overhead = len(json.dumps(base).encode("utf-8"))
        base["pad"] = "x" * (size - overhead)
        body = json.dumps(base).encode("utf-8")
        self.assertEqual(len(body), size)
        return body

    def test_a_request_of_exactly_1_mib_is_served(self):
        h = self.launch()
        _, agent = self.stepped(h)
        agent.send_body(self.padded_request(900001, MAX_REQUEST))
        reply = agent.wait_reply(900001, timeout=30.0)
        self.assertIn("result", reply, reply)

    def test_a_length_prefix_over_1_mib_closes_only_that_connection(self):
        h = self.launch()
        op, agent = self.stepped(h)
        for prefix in (MAX_REQUEST + 1, 2 * MAX_REQUEST, 0xFFFFFFFF):
            with self.subTest(prefix=prefix):
                agent.send_raw(struct.pack("<I", prefix))   # the body is never sent: it must not be waited for
                self.assertTrue(agent.wait_closed(10.0), "connection not closed")
                self.assertIsNotNone(op.call("get_speed"))   # the other connection still works
                agent = h.connect("agent")                    # and the slot is free again
                self.assertIn("input_id", agent.call("press_key", key="A", hold_ms=0))

    def test_an_oversized_request_logs_one_warning(self):
        h = self.launch()
        op, agent = self.stepped(h)
        agent.send_raw(struct.pack("<I", MAX_REQUEST + 1))
        self.assertTrue(agent.wait_closed(10.0))
        notes = [n for n in op.drain_notifications(quiet=0.5, limit=5.0) if n.get("method") == "log"]
        self.assertEqual([n["params"]["level"] for n in notes], ["warning"], notes)


# ---- Review Focus 3: a log storm ----

class LogStormTest(LimitTestCase):
    def test_1000_bad_requests_give_a_few_log_notifications(self):
        """Limit rejections are logged at most once per connection per second, with a count of the suppressed ones."""
        h = self.launch()
        op, agent = self.stepped(h)
        ids = [agent.request("press_key", {"key": "A", "hold_ms": 10001}) for _ in range(1000)]
        for rid in ids:
            reply = agent.wait_reply(rid, timeout=60.0)
            self.assertEqual(reply.get("error", {}).get("code"), INVALID_PARAMS, reply)
        notes = [n for n in op.drain_notifications(quiet=1.5, limit=10.0) if n.get("method") == "log"]
        self.assertGreaterEqual(len(notes), 1, "the first rejection must be logged")
        # one more rejection after the suppression window: its line reports how many were suppressed
        self.assertRpcError(INVALID_PARAMS, agent, "press_key", key="A", hold_ms=10001)
        notes += [n for n in op.drain_notifications(quiet=1.5, limit=10.0) if n.get("method") == "log"]
        self.assertLessEqual(len(notes), 6, "{} log notifications for 1,000 bad requests".format(len(notes)))
        suppressed = [n for n in notes if "suppressed" in n["params"]["message"].lower()]
        self.assertTrue(suppressed, "no log line reports a suppressed count: {}".format(notes))
        self.assertRegex(suppressed[0]["params"]["message"], r"\d+")

    def test_a_client_that_reconnects_in_a_loop_gives_a_bounded_stderr(self):
        """Each failed hello is a new connection, so a per-connection limit alone would not bound the output: the
        logger also caps warning/error records at 20 per second across all keys."""
        h = self.launch()
        time.sleep(0.5)
        before = len([l for l in h.stderr.decode("utf-8", "replace").splitlines() if l.strip()])
        started = time.monotonic()
        for _ in range(50):
            conn = h.open_raw()
            conn.send({"jsonrpc": "2.0", "id": 1, "method": "hello",
                       "params": {"protocol_version": "0.3", "role": "agent", "token": "wrong"}})
            conn.wait_closed(5.0)
        elapsed = time.monotonic() - started
        time.sleep(0.5)
        lines = [l for l in h.stderr.decode("utf-8", "replace").splitlines() if l.strip()]
        added = len(lines) - before
        bound = 20 * (int(elapsed) + 2) + 2
        self.assertGreaterEqual(added, 1, "the first failure must be logged")
        self.assertLessEqual(added, bound, "{} stderr lines for 50 failed hellos in {:.1f}s".format(added, elapsed))
        self.assertLess(added, 50, "the cap did not suppress anything: {}".format(lines[-5:]))


if __name__ == "__main__":
    unittest.main()
