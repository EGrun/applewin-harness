# Adapter Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** With `--state-dir`, the adapter keeps every disk the machine writes between runs in both modes, and in stepped mode saves the machine at a clean shutdown and restores it at the next start when nothing since would be undone.

**Architecture:**
- **`persistence.py` (new).** It owns the state directory: the lock, the persisted disk copies and their index, the session records, and the pure decision whether to resume.
- **`server.py`'s `Machine`.**
  - It mounts persisted copies.
  - It saves a drive whenever the harness reports a finished write burst (a watcher thread on the operator connection), before a disk leaves a drive, after a restore, and at shutdown.
  - In stepped mode it passes `checkpoint_dir` to the harness, takes a `session end` checkpoint at shutdown, and restores it at start.

  Nothing changes without `--state-dir`.

**Tech Stack:** Python 3.11+, standard library only in `persistence.py`, `mcp` 2.3.0, `unittest`; the harness's `checkpoint_dir` (this branch's earlier commits).

**Spec:** `docs/specs/persistence.md` §2, §3.3, §4, §5 and tests 6-16 of §6. Decision D18.

## Global Constraints

- `persistence.py` is standard library only and starts with `# SPDX-License-Identifier: MIT`.
- Without `--state-dir`, behaviour is exactly as today; every existing adapter test passes unchanged. `--fresh-disks` and `--fresh-machine` without `--state-dir` are usage errors.
- Every file written in the state directory is written to a temporary name in the same directory and then `os.replace`d.
- The state directory's `.lock` is an exclusive OS lock (`msvcrt.locking` on Windows) held for the adapter's lifetime; a second adapter exits with an error naming the directory.
- Realtime sessions take no machine state, ever: no `checkpoint_dir`, no checkpoint tools, no checkpoint at shutdown (user ruling 2026-11-02).
- The name `session end` is reserved for the adapter; the agent's `checkpoint` tool refuses it.
- `startup.json` `reason` values and their order are exactly the table in persistence spec §5.
- A failed disk save is logged as a warning (drive, key, cause), retried at the next burst and at shutdown, and never fails a tool call.
- Work on branch `persistent-checkpoints` (it holds the harness half). Don't merge or push without asking. Commit messages end with `Co-Authored-By: Claude Code <noreply@anthropic.com>`.
- Tests: `python -m unittest discover -s tests/adapter -v` with the Debug harness built from this branch (`HARNESS_EXE`, when set, must be absolute).

## Review Focus

1. **A realtime session that writes while no tool call is running** (the agent is thinking): the burst must still be saved within about a second of its end, not at the next tool call. Test in Task 2 (`test_realtime_write_is_saved_without_a_tool_call`).
2. **A restore (by the agent, or at startup) changes which disks the drives hold**: the adapter must re-learn each drive's key from `list_disks` and save the restored contents, or a later save would write one disk's bytes over another's copy. Test in Task 2 (`test_restore_saves_the_restored_disks`).
3. **A killed adapter** (no clean shutdown): the next stepped start must not restore an older end state over newer disk saves. Test in Task 3 (`test_no_restore_after_a_killed_session`).
4. **A failed `Machine.__init__`** (a harness that will not start, a locked directory) must not write `last_session.json`. Test in Task 3 (`test_a_failed_start_writes_no_session_record`).
5. **Two drives holding copies of the same original** (the agent inserts the boot disk in drive 2 too) share one key. Saving both into one copy would interleave two machines' views, so the second insert of an already-mounted key is refused with a clear error. Test in Task 2 (`test_the_same_original_in_both_drives_is_refused`).

## Decisions taken for the user (ledger)

- On first use the original is copied into `DIR/disks/` and the copy is always mounted, so a mounted disk's key is just the file name of its `original_path` (spec §4 updated).
- A key collision gets `-2` before the extension, because the harness tells formats by extension (spec §4 updated).
- `session_id` (a uuid per adapter run) ties `startup.json` to `last_session.json` for rule 3 (spec §5 updated, with the reason table).
- The same original mounted in both drives at once is refused when it would share a persisted copy (Review Focus 5), rather than saved twice.
- Saves come from a watcher thread blocking on the operator connection's notifications and taking `Machine.lock` per save; in stepped mode writes only happen inside locked tool calls, so the watcher just waits for the call to end.

---

### Task 1: `persistence.py`, the state directory and the resume decision

**Files:**
- Create: `adapter/applewin_mcp/persistence.py`
- Create: `tests/adapter/test_persistence.py` (unit tests, no harness)

**Interfaces:**
- Produces: `class StateDirLocked(Exception)`.
- Produces: `class StateDir` with these members:
  - `StateDir(path: str)`: makes `disks/`, `states/` and `archive/` if missing, and takes `.lock` (or raises `StateDirLocked`).
  - `close() -> None`: releases the lock.
  - `disks_dir: str`, `states_dir: str`: absolute paths.
  - `disk_for(original: str) -> str`: the persisted copy to mount. A path already inside `disks_dir` is returned as given. Otherwise the original is looked up in `index.json` by absolute path, compared case-insensitively. On first use it is copied in under a new key and an index entry is written. When the recorded `original_sha256` differs from the file's, a warning is logged naming both.
  - `key_of(mounted: str) -> str | None`: the file name when `mounted` is inside `disks_dir`, else `None`.
  - `save(key: str, write_to: Callable[[str], None]) -> None`: calls `write_to(tmp)` for a temporary path in `disks_dir` that does not exist yet. It then replaces `disks_dir/key` with it and updates the entry's `saved_at` and `sha256`. If `write_to` raises, it removes `tmp` and re-raises.
  - `archive_disks() -> str`: moves `disks/` to `archive/disks-<UTC YYYYMMDDTHHMMSSZ>/`, makes an empty `disks/`, and returns the archive path.
  - `disk_hashes() -> dict[str, str]`: key to sha256 of every persisted copy.
  - `read_json(name: str) -> dict | None` and `write_json(name: str, data: dict) -> None`, for `last_session.json` and `startup.json`: a missing or unreadable file reads as `None`, and writes are atomic.
- Produces: `resume_decision(run_mode: str, last_session: dict | None, previous_startup: dict | None, checkpoints: list[dict], disk_hashes: dict[str, str], fresh_machine: bool, fresh_disks: bool) -> tuple[str | None, str]`. It returns `(checkpoint_id or None, reason)`, with `reason` from the spec §5 table in its order. `checkpoints` are `list_checkpoints` rows (`checkpoint_id`, `compatible`).
- Produces: `index.json` entries `{"key", "original_path", "original_sha256", "first_used", "saved_at", "sha256"}`.

- [ ] **Step 1: Write the failing tests** in `tests/adapter/test_persistence.py` (temporary directories, small fake disk files):
  - `test_disk_for_copies_the_original_once`: the first call copies the original to `disks/<name>` and returns that path; a second call returns the same path without copying again (change the copy's bytes and check they stay); the original is unchanged.
  - `test_same_name_different_folders_get_suffixed_keys`: `a/Game.dsk` and `b/Game.dsk` get `Game.dsk` and `Game-2.dsk`; a third `c/Game.dsk` gets `Game-3.dsk`.
  - `test_a_path_inside_disks_is_mounted_as_given`, and `key_of` returns the file name for it and `None` for an outside path.
  - `test_a_changed_original_still_uses_the_copy_and_warns`: with `assertLogs` on the adapter's logger (whatever `log()` uses).
  - `test_save_replaces_the_copy_and_updates_the_index`: `sha256` and `saved_at` change; `write_to` receives a path that does not exist; a raising `write_to` leaves the copy and index as they were and no temporary file behind.
  - `test_archive_disks_moves_the_copies_aside`: after archiving, `disks/` is empty, the archive holds the old copies, and `disk_for` copies the original afresh.
  - `test_a_second_state_dir_is_locked`: a second `StateDir` on the same path raises `StateDirLocked` while the first is open, and opens after `close()`.
  - `test_json_files_are_atomic_and_tolerant`: `write_json` then `read_json` round-trips; a truncated file reads as `None`; no temporary file is left.
  - `test_resume_decision_reasons`: one sub-test per row of the spec §5 table, in order, each built so that only that row applies, and one where several apply to show the order (a realtime session with `--fresh-machine` gives `realtime`). `restored` returns the end checkpoint id; every other reason returns `None`.
- [ ] **Step 2: Run** `python -m unittest tests.adapter.test_persistence -v` (or `discover -s tests/adapter -p test_persistence.py`). Expected: FAIL (the module does not exist).
- [ ] **Step 3: Implement** `persistence.py`. sha256 is `hashlib` over the file. Timestamps are UTC ISO 8601 with `Z`. The lock is a file opened in `DIR/.lock`, locked with `msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)`; `OSError` from it becomes `StateDirLocked`. Logging goes through the same mechanism `server.py`'s `log()` uses; import it, or move `log` to a module both can import without a cycle.
- [ ] **Step 4: Run** the Step 2 command. Expected: PASS.
- [ ] **Step 5: Commit**
  ```bash
  git add adapter/applewin_mcp/persistence.py tests/adapter/test_persistence.py
  git commit -m "feat: the adapter's state directory and resume decision (D18)"
  ```

### Task 2: Disks that persist, in both modes

**Files:**
- Modify: `adapter/applewin_mcp/server.py` (`parse_args`, `session_params`, `Machine.__init__`, `insert_disk`, `eject_disk`, `restore_checkpoint`, `shutdown`, a disk-save watcher)
- Create: `tests/adapter/test_disk_persistence.py` (end to end, harness required; reuse `tests/adapter/test_mcp.py`'s `server_params`, `mcp_session`, `call`, `DOS_IMAGE`, `AdapterTestCase` patterns)

**Interfaces:**
- Consumes: Task 1's `StateDir`, `StateDirLocked`.
- Produces: the flags `--state-dir DIR`, `--fresh-disks` and `--fresh-machine` (the last is only parsed here and used in Task 3). The flags other than `--state-dir` without it are an `argparse` error.
- Produces: `Machine.state: StateDir | None`; `Machine.drive_keys: dict[int, str | None]` (drives 1 and 2); `Machine._save_drive(drive: int) -> None` (call with `self.lock` held: export to the key's copy, log and remember the drive on failure); `Machine._save_all() -> None`; `Machine._learn_drives() -> None` (rebuilds `drive_keys` from `list_disks`).

- [ ] **Step 1: Write the failing end-to-end tests** in `tests/adapter/test_disk_persistence.py`. Each test uses a fresh temporary state directory and a temporary copy of `DOS_IMAGE` as the original, and checks that the original's sha256 never changes:
  - `test_a_saved_program_survives_the_adapter` (spec test 6). In a stepped session with `--text-screen`, type `10 PRINT "KEPT"\n` and `SAVE KEPT\n`, then wait until the disk is idle (`wait` long enough for the burst to end). The persisted copy's sha256 has changed. Close the session. A new adapter on the same directory runs `LOAD KEPT\n` and `LIST\n`, and `read_screen` shows `10 PRINT "KEPT"`.
  - `test_the_same_in_a_realtime_session` (spec test 7), with `--realtime`.
  - `test_realtime_write_is_saved_without_a_tool_call` (Review Focus 1). In realtime, type the save, then make no tool call for 3 s and read the copy's sha256 from the test, not through the adapter: it has changed.
  - `test_insert_write_and_swap_back` (spec test 8). Insert a second temporary image into drive 1, `SAVE` to it, and insert the first again. Both persisted copies hold their programs (check with `LOAD` in a new adapter run, or by the copies' sha256 changing and the second run's `CATALOG`).
  - `test_fresh_disks_archives_and_starts_from_the_originals` (spec test 9). After a saving run, `--fresh-disks` leaves `archive/disks-*/` holding the saved copy, and `CATALOG` does not list `KEPT`.
  - `test_same_name_originals_get_different_keys` (spec test 10). Two originals named the same in different folders end up as `disks/<name>` and `disks/<stem>-2<ext>`.
  - `test_the_same_original_in_both_drives_is_refused` (Review Focus 5). With the boot disk in drive 1, `insert_disk` of the same original into drive 2 is a tool error saying the disk is already in drive 1.
  - `test_restore_saves_the_restored_disks` (Review Focus 2, spec test 16). Stepped: `checkpoint`, `SAVE KEPT`, `restore_checkpoint`. Right after the restore the persisted copy's sha256 equals its sha256 at the checkpoint.
  - `test_a_second_adapter_on_the_directory_exits` (spec test 15). While one adapter runs, a second with the same `--state-dir` fails to start, with an error naming the directory (in the adapter's stderr).
  - `test_without_state_dir_nothing_is_kept`. A run without `--state-dir` creates no state directory and behaves as before (one `SAVE`, then a new run's `CATALOG` shows no `KEPT`).
  - `test_fresh_flags_need_state_dir`. `--fresh-disks` alone exits with a usage error.
- [ ] **Step 2: Run** `python -m unittest discover -s tests/adapter -p test_disk_persistence.py -v`. Expected: FAIL (the flags are unknown).
- [ ] **Step 3: Implement.**
  - **`parse_args`** adds the flags.
  - **`Machine.__init__`**, with `--state-dir`:
    1. Open `StateDir` before launching the harness (a locked directory fails fast).
    2. With `--fresh-disks`, call `archive_disks()`.
    3. Mount `disk_for(--disk)` in `session_params`.
    4. After `create_session`, call `subscribe_disk_writes` on the operator connection, run `_learn_drives()`, and start a daemon watcher thread. It loops on `self.operator.next_notification(timeout=0.5)` until a stop event; on `disk_written` it takes `self.lock` and calls `_save_drive(params["drive"])`.
  - **`insert_disk`** maps the path through `disk_for`. It refuses a key already in the other drive (Review Focus 5), saves the outgoing disk of that drive first, then inserts and updates `drive_keys`. The reply still names the path the agent gave.
  - **`eject_disk`** saves first, then ejects and sets the drive's key to `None`.
  - **`restore_checkpoint`**, after the restore, runs `_learn_drives()` and then `_save_all()`.
  - **`shutdown`**, with a state directory: stop and join the watcher, take the lock and run `_save_all()` if the session was started, close the harness, then `state.close()`.
  - **The save itself:** `state.save(key, lambda tmp: self.rpc(self.operator, "export_disk", slot=6, drive=drive, dest_path=tmp))`. A write-protected drive (from `list_disks`) is never saved.
  - Without `--state-dir` none of this runs.
- [ ] **Step 4: Run** the Step 2 command. Expected: PASS. Then run the whole adapter suite. Expected: PASS (existing tests unchanged).
- [ ] **Step 5: Commit**
  ```bash
  git add adapter/applewin_mcp/server.py tests/adapter/test_disk_persistence.py
  git commit -m "feat: the adapter keeps every disk the machine writes between runs (D18)"
  ```

### Task 3: Resuming a stepped session, the tool texts and the docs

**Files:**
- Modify: `adapter/applewin_mcp/server.py` (`session_params` `checkpoint_dir`, start-up resume, shutdown end state, `checkpoint` reserved name, `list_checkpoints` text, checkpoint tool docstrings)
- Create: `tests/adapter/test_resume.py` (end to end, harness required)
- Modify: `tests/adapter/test_mcp.py` (only if a docstring assertion there changes; say which in the report)
- Modify: `adapter/README.md`, `CHANGELOG.md`, `docs/specs/persistence.md` (status), `docs/specs/client.md` §7.3 (map the spec §5 reason codes to the startup-note lines), `docs/api_spec.md` line 183 (a BEL control character replaced the `\a` of `%TEMP%\applewin-harness`; restore it)

**Interfaces:**
- Consumes: Task 1's `resume_decision`, `read_json`/`write_json`; Task 2's `state`, `_save_all`, `_learn_drives`.
- Produces: `Machine.session_id: str` (a uuid4 hex); `startup.json` and `last_session.json` exactly as spec §5; `SESSION_END = "session end"`; `KEEP_SESSION_ENDS = 5`.

- [ ] **Step 1: Write the failing end-to-end tests** in `tests/adapter/test_resume.py` (fresh temporary state directory each):
  - `test_a_clean_stepped_shutdown_is_resumed` (spec test 11). Run 1 (stepped, `--text-screen`) types `PRINT 1234\n` and closes. Afterwards `last_session.json` has `ended_cleanly: true`, an `end_checkpoint_id` and the run's `session_id`, and `list_checkpoints` in the next run shows it named `session end`. Run 2's `startup.json` has `restored` = that id and `reason` `restored`, and `read_screen` shows `1234` without typing anything.
  - `test_no_restore_with_fresh_machine`: `reason` `fresh_machine`; the checkpoint is still listed.
  - `test_no_restore_after_a_realtime_session`. Stepped, then realtime (whose `startup.json` reason is `realtime` and whose `last_session.json` has `end_checkpoint_id: null`), then stepped: `previous_realtime`.
  - `test_no_restore_after_a_killed_session` (Review Focus 3). A stepped clean run, then a stepped run killed (terminate the adapter process without closing the MCP session), then stepped: `previous_unclean`.
  - `test_no_restore_when_a_disk_changed`. After a clean stepped run, overwrite the persisted copy with the original's bytes: `disks_changed`.
  - `test_no_restore_of_another_build`. After a clean stepped run, edit the end checkpoint's `states/<id>/meta.json` `build`: `other_build`.
  - `test_only_five_session_ends_are_kept` (spec test 13). Seven clean stepped runs, with an agent checkpoint named `mine` made in the first. `list_checkpoints` shows 5 named `session end` (the newest) and `mine`.
  - `test_session_end_is_reserved`: the `checkpoint` tool with name `session end` is a tool error.
  - `test_realtime_takes_no_state` (spec test 14). A realtime run lists no checkpoint tools, `states/` stays empty, and `startup.json` reason is `realtime`.
  - `test_a_failed_start_writes_no_session_record` (Review Focus 4). Start with a nonexistent `--exe`: it fails, and no `last_session.json` is written. Then start with a locked state directory: same.
  - `test_list_checkpoints_marks_another_builds`. A checkpoint with an edited `build` is listed with `made by another build; cannot be restored`.
- [ ] **Step 2: Run** `python -m unittest discover -s tests/adapter -p test_resume.py -v`. Expected: FAIL.
- [ ] **Step 3: Implement.**
  - **`session_params`** adds `checkpoint_dir = state.states_dir` only in a stepped session with a state directory.
  - **`Machine.__init__`**, after Task 2's start-up and only with a state directory:
    1. Read `last_session.json` and the previous `startup.json`.
    2. Call `resume_decision(...)` with `list_checkpoints` (stepped only, else `[]`) and `disk_hashes()`.
    3. If it gives an id, `restore_checkpoint` it through the same code path as the agent's tool, so the adapter-state reset, `_learn_drives` and `_save_all` all happen.
    4. Write `startup.json` with the new `session_id`, then set `self.started = True`.
  - **`shutdown`**, only if `started`:
    1. After `_save_all()`, in a stepped session: `checkpoint {name: SESSION_END}` (on failure log it and use `None`), then delete all but the newest `KEEP_SESSION_ENDS` checkpoints named `SESSION_END`.
    2. Write `last_session.json` (stepped or realtime) with `disk_hashes()`.
    3. Then close the harness as Task 2 does.
  - **`checkpoint`** refuses `SESSION_END`.
  - **`list_checkpoints`** appends `  (made by another build; cannot be restored)` to rows with `compatible` false.
  - **Docstrings.** `checkpoint` says checkpoints are kept between sessions (replacing "they last as long as this session"), and that `session end` is reserved. `restore_checkpoint` adds that restoring also puts the disks back as they were then, undoing disk saves made since. `list_checkpoints` says it lists this and earlier sessions' checkpoints.
  - **Docs.**
    - `adapter/README.md`: the flags, the state directory layout, and what carries over in each mode.
    - `CHANGELOG.md` Unreleased: "Adapter: `--state-dir` keeps every disk the machine writes between runs; stepped sessions save their end state and resume it at the next start (`--fresh-machine`, `--fresh-disks`) (D18)."
    - `docs/specs/persistence.md`: status "implemented".
    - `docs/specs/client.md` §7.3: one startup-note line per reason code.
    - `docs/api_spec.md:183`: fix the BEL.
- [ ] **Step 4: Run** the Step 2 command. Expected: PASS. Then run the whole adapter suite, and `python -m unittest discover -s tests/python -v` once. Expected: PASS.
- [ ] **Step 5: Commit**
  ```bash
  git add adapter/applewin_mcp/server.py tests/adapter/test_resume.py tests/adapter/test_mcp.py adapter/README.md CHANGELOG.md docs/specs/persistence.md docs/specs/client.md docs/api_spec.md
  git commit -m "feat: a stepped session resumes where the last one ended (D18)"
  ```
