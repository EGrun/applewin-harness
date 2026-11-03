# SPDX-License-Identifier: MIT
"""The adapter's state directory and the resume decision (docs/specs/persistence.md §2, §4, §5).

Standard library only. Nothing here talks to the harness: server.py asks for the persisted copy of a disk, hands save()
a function that exports into a temporary path, and asks resume_decision() what to do at start.
"""

import datetime
import hashlib
import json
import logging
import os
import shutil
import uuid

try:
    import msvcrt
except ImportError:  # not Windows: the harness is Windows-only, but keep the adapter importable
    msvcrt = None
    import fcntl

_logger = logging.getLogger("applewin_mcp")


class StateDirLocked(Exception):
    """Another process holds the state directory's .lock."""


def _now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _same_path(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def _write_json_atomic(directory, name, data):
    tmp = os.path.join(directory, ".{}.{}.tmp".format(name, uuid.uuid4().hex))
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, os.path.join(directory, name))
    except BaseException:
        if os.path.exists(tmp):
            os.remove(tmp)
        raise


class StateDir:
    def __init__(self, path):
        self.path = os.path.abspath(path)
        self.disks_dir = os.path.join(self.path, "disks")
        self.states_dir = os.path.join(self.path, "states")
        self._archive_dir = os.path.join(self.path, "archive")
        for d in (self.disks_dir, self.states_dir, self._archive_dir):
            os.makedirs(d, exist_ok=True)
        self._lock = open(os.path.join(self.path, ".lock"), "a+b")
        try:
            self._lock.seek(0)
            if msvcrt:
                msvcrt.locking(self._lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                fcntl.flock(self._lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self._lock.close()
            raise StateDirLocked("state directory {} is in use by another process".format(self.path)) from None
        self._index = self._load_index()

    def close(self):
        if self._lock is None:
            return
        try:
            self._lock.seek(0)
            if msvcrt:
                msvcrt.locking(self._lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(self._lock.fileno(), fcntl.LOCK_UN)
        except OSError:
            pass
        self._lock.close()
        self._lock = None

    # index.json lives in disks/, so archiving the copies archives their index too.
    def _load_index(self):
        try:
            with open(os.path.join(self.disks_dir, "index.json"), encoding="utf-8") as f:
                data = json.load(f)
            return data if isinstance(data, list) else []
        except (OSError, ValueError):
            return []

    def _save_index(self):
        _write_json_atomic(self.disks_dir, "index.json", self._index)

    def key_of(self, mounted):
        if os.path.isfile(mounted) and _same_path(os.path.dirname(os.path.abspath(mounted)), self.disks_dir):
            return os.path.basename(mounted)
        return None

    def _new_key(self, name):
        stem, ext = os.path.splitext(name)
        taken = {e["key"].lower() for e in self._index}
        key, n = name, 1
        while key.lower() in taken or os.path.exists(os.path.join(self.disks_dir, key)):
            n += 1
            key = "{}-{}{}".format(stem, n, ext)
        return key

    def disk_for(self, original):
        original = os.path.abspath(original)
        if self.key_of(original) is not None:
            return original
        entry = next((e for e in self._index if _same_path(e["original_path"], original)), None)
        if entry is not None and os.path.isfile(os.path.join(self.disks_dir, entry["key"])):
            now_sha = sha256_of(original) if os.path.isfile(original) else None
            if now_sha is not None and now_sha != entry["original_sha256"]:
                _logger.warning("original disk {} changed since first use (was sha256 {}, now {}); "
                                "still using the persisted copy {}; use --fresh-disks to start over".format(
                                    original, entry["original_sha256"], now_sha, entry["key"]))
            return os.path.join(self.disks_dir, entry["key"])
        if entry is not None:
            self._index.remove(entry)
        key = self._new_key(os.path.basename(original))
        target = os.path.join(self.disks_dir, key)
        stem, ext = os.path.splitext(key)
        tmp = os.path.join(self.disks_dir, "{}.tmp-{}{}".format(stem, uuid.uuid4().hex, ext))
        try:
            shutil.copyfile(original, tmp)
            os.replace(tmp, target)
        except BaseException:
            if os.path.exists(tmp):
                os.remove(tmp)
            raise
        digest = sha256_of(target)
        now = _now()
        self._index.append({"key": key, "original_path": original, "original_sha256": digest, "first_used": now,
                            "saved_at": now, "sha256": digest})
        self._save_index()
        return target

    def save(self, key, write_to):
        stem, ext = os.path.splitext(key)
        # the harness tells disk formats by extension, so the temporary name keeps it
        tmp = os.path.join(self.disks_dir, "{}.tmp-{}{}".format(stem, uuid.uuid4().hex, ext))
        try:
            write_to(tmp)
            digest = sha256_of(tmp)
            os.replace(tmp, os.path.join(self.disks_dir, key))
        except BaseException:
            if os.path.exists(tmp):
                os.remove(tmp)
            raise
        for e in self._index:
            if e["key"] == key:
                e["saved_at"] = _now()
                e["sha256"] = digest
                break
        self._save_index()

    def archive_disks(self):
        dest = os.path.join(self._archive_dir, "disks-" + datetime.datetime.now(datetime.timezone.utc).strftime(
            "%Y%m%dT%H%M%SZ"))
        n = 1
        while os.path.exists(dest + ("" if n == 1 else "-{}".format(n))):
            n += 1
        if n > 1:
            dest += "-{}".format(n)
        os.replace(self.disks_dir, dest)
        os.makedirs(self.disks_dir)
        self._index = []
        return dest

    def disk_hashes(self):
        out = {}
        for name in sorted(os.listdir(self.disks_dir)):
            p = os.path.join(self.disks_dir, name)
            if name != "index.json" and ".tmp-" not in name and os.path.isfile(p):
                out[name] = sha256_of(p)
        return out

    def read_json(self, name):
        try:
            with open(os.path.join(self.path, name), encoding="utf-8") as f:
                data = json.load(f)
            return data if isinstance(data, dict) else None
        except (OSError, ValueError):
            return None

    def write_json(self, name, data):
        _write_json_atomic(self.path, name, data)


def resume_decision(run_mode, last_session, previous_startup, checkpoints, disk_hashes, fresh_machine, fresh_disks):
    """Returns (checkpoint_id or None, reason); reasons and their order are the table in persistence.md §5."""
    if run_mode == "realtime":
        return None, "realtime"
    if fresh_machine:
        return None, "fresh_machine"
    if fresh_disks:
        return None, "fresh_disks"
    if last_session is None:
        return None, "first_session"
    if (not last_session.get("ended_cleanly") or previous_startup is None
            or previous_startup.get("session_id") != last_session.get("session_id")):
        return None, "previous_unclean"
    if last_session.get("run_mode") != "stepped":
        return None, "previous_realtime"
    cp_id = last_session.get("end_checkpoint_id")
    row = next((c for c in checkpoints if c.get("checkpoint_id") == cp_id), None) if cp_id else None
    if row is None:
        return None, "no_end_state"
    if not row.get("compatible"):
        return None, "other_build"
    if disk_hashes != last_session.get("disks"):
        return None, "disks_changed"
    return cp_id, "restored"
