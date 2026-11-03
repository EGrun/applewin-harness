# Machine and disk persistence spec

**Status:** Proposed (D18). §3.1–3.2 implemented. It changes the harness and the adapter; the agent client (`docs/specs/client.md`) uses it.

## 1. Purpose

A long goal outlives one harness process. Today nothing of the machine does: checkpoints "live and die with the session's work directory" and `close_session` deletes the working disk copies (`docs/api_spec.md`). The user's rulings (2026-11-02) are:

- **Stepped sessions:** saved machine states carry over between sessions and are available to the next run.
- **Realtime sessions:** only the game's own saving is available, such as saving to and loading from a disk with the game's features. No machine states.
- **Both:** modified disks survive harness restarts.

This spec makes all three true. It adds no agent tool and changes no rule of D4: the agent's checkpoint tools still exist only in stepped sessions; they just remember across processes.

### Non-goals
- No machine state in realtime, in any form (the adapter neither takes nor restores one).
- No change to the disk formats or to how AppleWin writes them.
- No sharing of one state directory between two processes at once.

## 2. The state directory

One directory per program being played, chosen by the operator (the client passes the campaign's, `docs/specs/client.md` §7):

```
<state-dir>/
  disks/              the persisted disk images, one per original image (§4)
    index.json        [{ "key", "original_path", "original_sha256", "saved_at", "sha256" }]
  states/             stepped-session checkpoints (§3), one directory each
  last_session.json   how the last session ended (§5)
  startup.json        what this session started from (§5), written at start
  archive/            what --fresh-disks and --fresh-machine moved aside (never deleted by the code)
  .lock               held while a process uses the directory
```

- **One user at a time.** The adapter takes `.lock` (an exclusive OS lock, released by the OS if the process dies) before anything else. If it is held, the adapter exits with an error naming the directory.
- Every file the adapter or harness writes here is written to a temporary name in the same directory and then renamed over the old one, so a crash never leaves half a file.

## 3. Harness: checkpoints that persist (stepped only)

### 3.1 API change
`create_session` gains `checkpoint_dir?: string`.

- **Without it** nothing changes: checkpoints live in the work directory and die with the process.
- **With it**, each checkpoint is stored in `<checkpoint_dir>/<checkpoint_id>/`: the AppleWin save state, the copies of the working disks and `meta.json`, as today. `meta.json` gains three fields:
  - `"build"`: the build that made it (§3.2);
  - `"run_mode"`: `"stepped"` or `"realtime"`, informational;
  - per disk, `"original_path"` (the image the session's working copy came from, as `list_disks` reports it) and `"state_path"` (the disk's path exactly as the save state holds it, D10's short path included).

  A checkpoint is written to `<checkpoint_id>.tmp/` and renamed when complete; leftover `.tmp` directories and a leftover `pending.yaml` are deleted at `create_session`.
- `checkpoint_dir` must be an absolute path (`-32602` otherwise). It is created if missing. A second process using the same directory gets **`-32003` resource locked** at `create_session`: the first holds `<checkpoint_dir>/.lock` open without sharing until it exits.
- `create_session` reads every complete checkpoint in the directory: `cp-<n>` directories whose `meta.json` parses and whose save state and disk copies exist. Others are skipped (logged as warnings) and left alone. `list_checkpoints` lists them with the ones made in this process, oldest first, and adds `"compatible": bool` (§3.2) to every entry. New ids continue from the highest number of any `cp-<n>` directory, complete or not, so an id is never reused within a directory.
- **Restoring a checkpoint made by an earlier process.** Its disks' working paths were in that process's work directory, which is gone, and the save state names those paths. So the harness restores it onto new working copies: each disk copy is copied to `<work>/restore-<k>/disks/<working file name>`, a copy of the save state is written to `<work>/restore-<k>/state/state.yaml` with each disk's `Absolute Path` replaced by its new working copy (written as AppleWin's own save code writes paths), and that copy is loaded. The new working copies are mounted with their `original_path` from `meta.json`, so `list_disks` and `export_disk` behave as after any restore. A checkpoint made in this process restores as today.
- `close_session` leaves the directory as it is. `delete_checkpoint` deletes the checkpoint's directory.
- The limit of 256 checkpoints counts the ones in the directory.
- `checkpoint_dir` is accepted in either run mode by the harness (it is a mechanism); the adapter only ever passes it in stepped mode (§3.3).

### 3.2 Builds
A save state from another harness or AppleWin build may not load, and a failed load leaves the session unusable (`-32004`, api_spec "Failure in or after the load"). So:

- `build` is fixed at compile time: `<harness version>+<AppleWin submodule commit, 12 hex digits>`, for example `0.1.0+6c579da5bd94`. The build reads the commit with `git`; without git it is `unknown`, and `0.1.0+unknown` is compatible only with itself.
- `restore_checkpoint` of a checkpoint whose `build` differs returns a new error, **`-32011` checkpoint from another build**, with both builds in the message. Nothing is loaded and the session stays usable.
- `list_checkpoints` shows `compatible: false` for such checkpoints. Deleting them is the operator's choice; nothing is deleted automatically.

### 3.3 Adapter
- New flag `--state-dir DIR`. In a stepped session the adapter passes `checkpoint_dir = DIR/states` to `create_session`. In a realtime session it passes no `checkpoint_dir`, and the checkpoint tools are not offered (as now).
- The agent's `checkpoint`, `restore_checkpoint`, `list_checkpoints` and `delete_checkpoint` work as now. Their descriptions add two sentences: checkpoints are kept between sessions; restoring one also puts the disks back as they were at that checkpoint (api_spec already says so for the drives), which replaces any disk saves made since.
- `list_checkpoints` shows `compatible` only when it is false ("made by another build; cannot be restored").

## 4. Adapter: disks that persist (both modes)

The harness never writes an original image (D5) and already provides what is needed: `export_disk` flushes and copies a working image, and `disk_written` (after `subscribe_disk_writes`) says when a drive has new writes. The adapter keeps one persisted copy per original image in `DIR/disks/`.

- **Key.** An original image is identified by its absolute path. Its key is its file name; if `index.json` already has that file name for a different original path, the key gets `-2`, `-3` and so on. `index.json` records the original's path and sha256 at first use.
- **Mounting.** For `--disk` at start, and for every `insert_disk`, the adapter looks the original up in `index.json`. If a persisted copy exists, it mounts that copy instead (the harness copies it into its work directory, as for any image, so the persisted copy is never opened for writing). Otherwise it mounts the original and adds an index entry. A `write_protected` insert is mounted the same way but never saved.
- **Saving.** The adapter subscribes to disk writes at start. After each `disk_written` for a drive, and for every drive at shutdown, it calls `export_disk` to a temporary file in `DIR/disks/` and renames it over the key's copy, then updates `saved_at` and `sha256`. Before `eject_disk`, or an `insert_disk` that replaces a disk, it saves the outgoing disk first. After a `restore_checkpoint` it saves every drive, so the persisted copies follow the machine.
- **What a crash loses.** At most the writes of the burst in progress: each burst is saved when it ends (about one emulated second after the last write, api_spec `disk_written`).
- **A changed original.** If the original's sha256 differs from the one recorded, the persisted copy is still used (it holds the game's saves) and the adapter logs a warning naming both files. `--fresh-disks` is how to start over.
- **`--fresh-disks`** moves `DIR/disks/` to `DIR/archive/disks-<UTC timestamp>/` before mounting, so every disk starts again from its original.
- **Failures.** A failed save is logged as a warning with the drive, the key and the cause, and retried at the next write burst and at shutdown. It never fails an agent tool call.
- The agent's `export_disk` is unchanged: it still writes a copy where the agent asks.

## 5. Adapter: resuming a stepped session

- **At shutdown of a stepped session** (the MCP session closes or the adapter is stopped normally), the adapter, in this order: saves every drive (§4); takes `checkpoint {name: "session end"}`; deletes all but the newest 5 checkpoints named `session end` (the agent's own checkpoints are never deleted automatically); writes `last_session.json`:

  ```json
  { "run_mode": "stepped", "ended_cleanly": true, "end_checkpoint_id": "cp-41",
    "ended_at": "2026-11-02T21:55:03Z", "disks": { "<key>": "<sha256>" } }
  ```

- **At shutdown of a realtime session** it saves every drive and writes `last_session.json` with `"run_mode": "realtime"`, `"end_checkpoint_id": null`. No machine state is taken.
- **A session that ends any other way** (a crash, a kill) writes nothing at the end. The `last_session.json` of an earlier session is left, but `startup.json` of the dead session (below) shows it never ended, which the next start detects.
- **At start of a stepped session** the adapter restores the previous session's end state when all of these hold, and otherwise boots the machine fresh with the persisted disks:
  1. `--fresh-machine` was not given;
  2. `last_session.json` says `stepped`, `ended_cleanly`, and names an end checkpoint;
  3. no later session started after it (the previous `startup.json` belongs to that same session);
  4. the checkpoint exists and is `compatible`;
  5. every persisted disk's sha256 matches `last_session.json`'s `disks`, so no disk changed since (for example in a realtime session in between, whose disk saves a restore would undo).
- **At start of a realtime session** the machine always boots fresh with the persisted disks.
- `startup.json` records what happened: `{ "run_mode", "started_at", "restored": "<checkpoint id>" | null, "reason": "<which rule above decided>" }`. The client reads it to tell the agent and the person (`docs/specs/client.md` §7).
- **`--fresh-machine`** boots fresh even when the rules would restore; checkpoints stay available to the agent in a stepped session.

## 6. Tests

**Harness** (`HarnessTests`):
1. With `checkpoint_dir`, a checkpoint made in one process is listed and restores in a second process, with the same state checksum as straight after it was made.
2. Ids continue from the highest existing one; `delete_checkpoint` removes the directory; the 256 limit counts persisted checkpoints.
3. A checkpoint whose `meta.json` names another build: listed with `compatible: false`, `restore_checkpoint` returns `-32011`, and the session still steps and checkpoints normally.
4. A leftover `.tmp` directory is removed at `create_session` and never listed.
5. Without `checkpoint_dir`, behaviour is exactly as before (existing checkpoint tests unchanged).

**Adapter** (`tests/adapter`, harness required):
6. A program writes to the disk (the DOS 3.3 System Master: `SAVE` a BASIC line); after the write burst the persisted copy's sha256 changes; a new adapter process with the same `--state-dir` mounts it and `LOAD` returns the line. The original image is byte-for-byte unchanged.
7. The same in a realtime session.
8. `insert_disk` of a second image, a write, a swap back: both persisted copies hold their writes.
9. `--fresh-disks` moves the old copies to `archive/` and mounts the originals.
10. Two originals with the same file name in different folders get different keys.
11. Stepped: a clean shutdown writes a `session end` checkpoint and `last_session.json`; the next stepped start restores it (`startup.json.restored` set, the screen as it was).
12. No restore when: `--fresh-machine`; the previous session was realtime; the previous session crashed (killed adapter); a disk's sha256 changed; the checkpoint is from another build. Each sets `startup.json.reason`.
13. Only the newest 5 `session end` checkpoints are kept; an agent-named checkpoint survives.
14. Realtime: no `checkpoint_dir` is sent, no checkpoint tools are listed, and no state is taken at shutdown.
15. A second adapter on a locked `--state-dir` exits with an error naming it.
16. After `restore_checkpoint` the persisted disks equal the checkpoint's disks.

## 7. Open points
1. **What `session end` means in a realtime game paused by the game.** Nothing: realtime takes no states, by the ruling. The game's own save is the only carry-over there.
2. **Size.** A checkpoint is a save state plus a copy of each disk (a few hundred KB). Five session-end states plus the agent's own are small; no size limit is set beyond the 256 count.
3. **AppleWin upgrades.** Moving the AppleWin submodule pin makes every older checkpoint incompatible (§3.2). The disks carry over regardless; a stepped session then boots fresh. A conversion path is not planned.
