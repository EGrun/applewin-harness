# Persistent Checkpoints Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harness checkpoints can live in a directory the operator names, survive the process, and restore in a later process; a checkpoint from another build is refused without harming the session.

**Architecture:** `CheckpointStore` gains a persistent mode: it opens and locks a directory, reads the checkpoints already there, and writes new ones atomically with the build that made them. `Session` passes `create_session.checkpoint_dir` to it, refuses other builds with `-32011`, and restores a checkpoint made by an earlier process onto new working copies with a rewritten copy of the save state, since the old working paths are gone. Nothing changes without `checkpoint_dir`.

**Tech Stack:** C++20, MSVC, Win32, nlohmann::json, doctest (`tests/HarnessTests.vcxproj`), Python `unittest` over the pipe (`tests/python`).

**Spec:** `docs/specs/persistence.md` §3.1, §3.2 and tests 1-5 of §6 (the harness part; §3.3, §4 and §5 are the adapter's and not in this plan). Decision D18 in `docs/decisions.md`.

## Global Constraints

- Every new source file starts with `// SPDX-License-Identifier: MIT` (C++) or `# SPDX-License-Identifier: MIT` (Python) (D6).
- No AppleWin patches (D12): everything is harness code around AppleWin's own save and load.
- Ruling D7 holds for every new path: a failure found before the load leaves the machine, the drives and the session as they were and the session usable; only a failure in or after `Snapshot_LoadState` makes it unusable.
- Without `checkpoint_dir`, behaviour is exactly as today; every existing test passes unchanged.
- `build` is `<harness version>+<AppleWin submodule commit, 12 lowercase hex digits>`, or `<harness version>+unknown` when git cannot tell; it is compatible only with an identical string.
- New error: `-32011` "checkpoint from another build", message naming both builds. Locked directory: `-32003`. Relative `checkpoint_dir`: `-32602`.
- A checkpoint is built in `<checkpoint_dir>/<id>.tmp/` and renamed to `<id>/` when complete.
- Work on branch `persistent-checkpoints` from `main`. Don't merge or push without asking. Commit messages end with `Co-Authored-By: Claude Code <noreply@anthropic.com>`.
- Build both configurations: `msbuild AppleWinHarness.sln -m -p:Configuration=Debug -p:Platform=x64` and the same with `Release` (find MSBuild with `vswhere -latest -find MSBuild\**\Bin\MSBuild.exe`).
- Test commands: `bin\x64\Debug\HarnessTests.exe -tse=windowed,audio`; `python -m unittest discover -s tests/python -v`; `python -m unittest discover -s tests/adapter -v`. `HARNESS_EXE`, when set, must be an absolute path.

## Review Focus

1. **A restore from an earlier process when `%TEMP%` needs D10's short path** (a CJK temp folder with an 8.3 name): the rewritten save state must name the new working copies in the form AppleWin loads. Test in Task 2 (`test_restore_from_an_earlier_process_under_a_cjk_temp`).
2. **A process that dies while writing a checkpoint** leaves only `<id>.tmp/`, which the next `create_session` deletes; a half-written `cp-<n>/` never appears in `list_checkpoints`. Test in Task 1 (`persist: a failed Create leaves no directory and does not use up an id`) and Task 2 (`test_leftovers_are_removed_and_broken_directories_skipped`).
3. **A restore from an earlier process that fails before the load** (a disk copy deleted) gets `-32004`, and the drives, the machine and the session are as before (D7). Test in Task 2.
4. **A checkpoint taken after a restore from an earlier process** records the new working copies and restores in the same process (the in-process path) and again in a third process. Test in Task 2.
5. **A harness killed without `close_session`** releases its lock: the next process opens the directory. Test in Task 2.

## Decisions taken for the user (ledger)

- The new per-checkpoint fields go into the existing `meta.json`, not a second `checkpoint.json` (the spec is updated to match).
- A checkpoint made by an earlier process is restored onto new working copies under `<work>/restore-<k>/` with a rewritten copy of its save state, rather than by recreating the old process's paths, which may be another live process's.
- The AppleWin commit comes from `git` at build time; without git the build is `unknown`.
- `.tmp` directories are deleted before ids are counted, so they never reserve an id; any `cp-<n>` directory, complete or not, does.
- `checkpoint_dir` errors at `create_session`: relative or not a directory `-32602`, locked `-32003`, any other file-system failure `-32000` with the cause.

---

### Task 1: The build stamp and a persistent `CheckpointStore`

**Files:**
- Modify: `build/Harness.props` (an `AppleWinCommit` property and an `APPLEWIN_COMMIT` define)
- Modify: `src/host/harness_version.h`, `src/host/harness_version.cpp` (add `BuildId`)
- Modify: `src/server/checkpoint_store.h`, `src/server/checkpoint_store.cpp`
- Create: `tests/checkpoint_store_test.cpp` (and add it to `tests/HarnessTests.vcxproj`)

**Interfaces:**
- Produces: `const char* harness::BuildId();`, the build string of Global Constraints.
- Produces: `struct harness::CheckpointDirLocked : std::runtime_error` (thrown when another process holds the directory).
- Produces: `struct CheckpointStore::DiskSource { int drive; std::filesystem::path workingPath; std::filesystem::path originalPath; std::string statePath; };`
- Produces, on `CheckpointStore::DiskCopy`: `std::filesystem::path originalPath; std::string statePath;`
- Produces, on `CheckpointStore::Entry`: `std::string build; std::string runMode; bool foreign = false;` (`foreign`: read from the directory at open, not made by this store).
- Produces: `CheckpointStore(std::filesystem::path workDir, std::filesystem::path persistentDir);`. It creates `persistentDir` if missing, takes `<persistentDir>/.lock`, deletes `*.tmp/` directories and `pending.yaml`, and reads every complete `cp-<n>/`. It throws `CheckpointDirLocked`, or `std::runtime_error` naming the cause. The store is not copyable; it holds the lock until destroyed.
- Changes: `Entry Create(std::string name, const SnapshotMeta& meta, const std::vector<DiskSource>& disks, std::string runMode);` replaces the `DiskInfo` overload. Both modes record `build = BuildId()`, `runMode`, `originalPath` and `statePath`.
- Produces: `const std::vector<std::string>& Skipped() const;`, one message per `cp-<n>` directory skipped at open.
- Keeps: `CheckpointStore(std::filesystem::path workDir)` (ephemeral, as today), and `Get`, `List`, `Delete`, `PrepareRestore`, `RestoreDisks`, `CommitRestore`, `RollbackRestore` with their current behaviour.

- [ ] **Step 1: Write the failing tests** in `tests/checkpoint_store_test.cpp`. Use a temporary directory per test, plain files as disk images and a hand-made `SnapshotMeta`. Each `TEST_CASE` name starts `persist:`:
  - `persist: BuildId is the version, a plus and twelve hex digits or unknown`: matches `^\d+\.\d+\.\d+\+([0-9a-f]{12}|unknown)$` and starts with `Version()` + `"+"`.
  - `persist: a checkpoint is read back by a second store on the same directory`: store A creates `"first"` with two `DiskSource`s, then is destroyed; store B on the same directory lists one entry. It has the same id, name, `createdAt`, every `SnapshotMeta` field, `build == BuildId()`, `runMode == "stepped"`, and per disk the drive, `originalPath`, `statePath` and a copy whose bytes equal the source. `foreign` is true in B and was false in A.
  - `persist: ids continue past the highest cp directory, complete or not`: the directory holds a complete `cp-3/` (from a store) and a `cp-7/` with an unparsable `meta.json`. A new store lists only `cp-3`, `Skipped()` names `cp-7`, `cp-7/` is still on disk, and the next `Create` returns `cp-8`.
  - `persist: leftover tmp directories and pending.yaml are deleted at open`: `cp-9.tmp/` and `pending.yaml` are gone after open, and the next id is `cp-1`.
  - `persist: a second store on a locked directory throws CheckpointDirLocked`: with store A alive, B throws `CheckpointDirLocked`; after A is destroyed, B opens.
  - `persist: a failed Create leaves no directory and does not use up an id`: a `DiskSource` whose working file is missing makes `Create` throw. Neither `cp-1/` nor `cp-1.tmp/` exists, and the next successful `Create` returns `cp-1`.
  - `persist: Delete removes the directory and the limit counts entries read at open`: `Delete("cp-1")` removes `cp-1/`. With 256 complete entries on disk (written by a store), a fresh store's `Create` throws `std::runtime_error` mentioning `256`.
  - `persist: the ephemeral store records build, run mode and paths too`: `CheckpointStore(workDir)` entries carry `build == BuildId()`, the given `runMode`, `originalPath` and `statePath`, and `foreign == false`.
- [ ] **Step 2: Build and run** `bin\x64\Debug\HarnessTests.exe -tc="persist:*"`. Expected: the build fails (`BuildId`, `DiskSource` and the two-path constructor are not declared).
- [ ] **Step 3: Implement.**
  - **`build/Harness.props`:** a target that runs before `ClCompile`. It runs `git -C "$(HarnessRoot)external\AppleWin" rev-parse --short=12 HEAD` with `ConsoleToMSBuild="true"`, `IgnoreExitCode="true"` and `StandardErrorImportance="low"`, and sets `AppleWinCommit` to the output when it is 12 hex digits, else `unknown`. It adds `APPLEWIN_COMMIT="$(AppleWinCommit)"` to the preprocessor definitions next to `HARNESS_VERSION` (find where `HARNESS_VERSION` is defined and follow that form).
  - **`harness_version.cpp`:** `BuildId()` returns `HARNESS_VERSION "+" APPLEWIN_COMMIT`, with the same `#error` guard style.
  - **The persistent store:** `entries_` and `nextNumber_` as now. The lock is a `CreateFileW` handle on `.lock`, opened with share mode 0 and `OPEN_ALWAYS`. A sharing violation becomes `CheckpointDirLocked`; the handle closes in the destructor (delete copy, define move or forbid it). `Create` builds `<id>.tmp/` (the state moved in, the disk copies, `meta.json`), then renames it to `<id>/`, and removes `<id>.tmp/` on any failure. `meta.json` adds `build`, `run_mode`, and per disk `original_path` and `state_path`. Reading at open parses exactly what `Create` writes, and requires `state.yaml` and every disk copy to exist. The ephemeral mode keeps `<workDir>/checkpoints/` and needs no lock.
- [ ] **Step 4: Run** `bin\x64\Debug\HarnessTests.exe -tc="persist:*"`. Expected: PASS. Then fix the session's call to `Create` (it compiles against the new signature: pass `DiskSource`s with `originalPath = OriginalOf(workingPath)`, `statePath` empty for now, and the run mode). Build Debug and Release, and run the full `HarnessTests.exe -tse=windowed,audio` and `python -m unittest discover -s tests/python -v`. Expected: PASS, nothing else changed.
- [ ] **Step 5: Commit**
  ```bash
  git add build/Harness.props src/host/harness_version.h src/host/harness_version.cpp src/server/checkpoint_store.h src/server/checkpoint_store.cpp src/server/session.cpp tests/checkpoint_store_test.cpp tests/HarnessTests.vcxproj
  git commit -m "feat: checkpoints can persist in a directory and record their build (D18)"
  ```

### Task 2: `create_session.checkpoint_dir`, `-32011`, restoring a checkpoint from an earlier process, and the API docs

**Files:**
- Modify: `src/server/session.h`, `src/server/session.cpp`
- Modify: `src/server/protocol.h` (or wherever the `-320xx` constants live: add `-32011`)
- Modify: `src/host/emulator.h`, `src/host/emulator.cpp` (expose the save state's image names, a rewriter, and `ImagePathForAppleWin`)
- Modify: `tests/snapshot_test.cpp` (the rewriter round trip)
- Create: `tests/python/test_persistent_checkpoints.py`
- Modify: `docs/api_spec.md`, `CHANGELOG.md`, `docs/specs/persistence.md` (status line)

**Interfaces:**
- Consumes: Task 1's `BuildId`, `CheckpointDirLocked`, `DiskSource`, `Entry::{build, runMode, foreign}`, `DiskCopy::{originalPath, statePath}`, the two-path constructor and `Skipped()`.
- Produces, in `emulator.h` (namespace `harness`):
  - `std::string ImagePathForAppleWin(const std::filesystem::path& image);` (today's internal helper, now declared: the path string AppleWin is given at insert, D10's short path included).
  - `std::vector<std::pair<std::string, std::string>> SaveStateImages(const std::filesystem::path& stateFile);` returns `{Filename, Absolute Path}` for each disk the save state names, decoded as `ImagesNamedBy` decodes them.
  - `void RewriteSaveStateImages(const std::filesystem::path& from, const std::filesystem::path& to, const std::map<std::string, std::string>& absoluteOldToNew);` copies the save state, replacing each `Absolute Path:` value found in the map by its new value, encoded the way AppleWin's `YamlSaveHelper::SaveString` writes a string (read it in `external/AppleWin/source/YamlHelper.cpp`). Every other byte is unchanged. It throws `std::runtime_error` if any key in the map is not found.
- Produces, on the wire: `create_session.checkpoint_dir`, `list_checkpoints[].compatible`, `restore_checkpoint` → `-32011`.

- [ ] **Step 1: Write the failing C++ test** in `tests/snapshot_test.cpp`, `ckpt: RewriteSaveStateImages replaces only the named absolute paths`. Take a save state written by `SaveSnapshot` with a disk in drive 1. Rewrite its absolute path to `C:\a b\c#d\e.dsk`. `SaveStateImages` of the result returns that path exactly, with the same `Filename`. Every line other than that `Absolute Path:` line is byte-identical. A map key that is not in the file throws.
- [ ] **Step 2: Write the failing pipe tests** in `tests/python/test_persistent_checkpoints.py`, a `CheckpointTestCase` subclass (reuse `tests/python/test_checkpoints.py`'s helpers: `dos_disks`, `work_dir`, `sha256_file`, `frame_hash`). Each test uses a fresh temporary `checkpoint_dir` and starts harness processes one after another:
  - `test_a_checkpoint_survives_the_process`. Process 1 (stepped, the DOS 3.3 disk, booted, a BASIC line typed) checkpoints with name `"first"`, records `get_state_checksum` and the frame hash, and closes. `<dir>/cp-1/meta.json` has `build`, `run_mode` `"stepped"`, `original_path` and `state_path`. Process 2 on the same directory lists `cp-1` with name `"first"` and `compatible: true`. Its restore returns the same `emulated_cycles`, `get_state_checksum` and frame hash. `list_disks` shows `original_path` = the DOS image and `working_path` under process 2's `work_dir(...)\restore-1\disks\`. `export_disk` gives the bytes of `<dir>/cp-1/disk1-*`.
  - `test_the_disks_come_back_as_they_were`. Process 1 checkpoints, then `SAVE`s a program to disk, then closes. Process 2 restores and runs `CATALOG`: the saved program is not listed.
  - `test_ids_continue_and_delete_removes_the_directory`. Process 2 checkpoints and gets `cp-2`, then deletes `cp-1` (`<dir>/cp-1` is gone). Process 3 lists only `cp-2`, and its next checkpoint is `cp-3`.
  - `test_another_build_gets_32011_and_the_session_stays_usable`. Edit `<dir>/cp-1/meta.json`'s `build` to `"0.0.0+000000000000"`. The list shows `compatible: false`. Restore gets `-32011` with both builds in the message. `step_frames`, `checkpoint` and the restore of the new checkpoint then work.
  - `test_leftovers_are_removed_and_broken_directories_skipped`. Before process 2 the directory gains `cp-9.tmp/`, `pending.yaml`, and `cp-5/` holding only a `meta.json` of `{}`. After `create_session`, `cp-9.tmp/` and `pending.yaml` are gone, `cp-5/` is still there and not listed, and the next checkpoint is `cp-6`.
  - `test_a_second_process_on_the_directory_gets_32003`. With process 1's session open on the directory, process 2's `create_session` with it gets `-32003`. Process 2's `create_session` without `checkpoint_dir` then succeeds.
  - `test_a_killed_process_releases_the_directory`. Kill process 1 without `close_session`; process 2 opens the directory.
  - `test_a_relative_checkpoint_dir_gets_32602`.
  - `test_a_failed_restore_from_an_earlier_process_leaves_the_session_as_it_was` (Review Focus 3). Delete `<dir>/cp-1/disk1-*`. Restore gets `-32004`. Afterwards `list_disks`, `get_state_checksum` and the frame are as before the call, and `step_frames` works.
  - `test_a_checkpoint_after_a_restore_from_an_earlier_process` (Review Focus 4). Process 2 restores `cp-1`, types, checkpoints `cp-2`, types more, and restores `cp-2` in the same process: the checksum matches the one taken right after `cp-2`. Process 3 restores `cp-2` with the same checksum.
  - `test_restore_from_an_earlier_process_under_a_cjk_temp` (Review Focus 1). Process 2 runs with `TEMP` and `TMP` pointing at a CJK-named folder that has an 8.3 short name, using the helper `tests/python/test_logging.py` uses for its CJK temp test. Its restore of `cp-1` matches the checksum. Skip, with the same reason as that test, where 8.3 names are off.
  - `test_a_realtime_checkpoint_restores_in_a_later_stepped_session`. Process 1 is realtime with `checkpoint_dir`, and the checkpoint records `run_mode` `"realtime"`. Process 2 is stepped and restores it.
- [ ] **Step 3: Run** `bin\x64\Debug\HarnessTests.exe -tc="ckpt: Rewrite*"` and `python -m unittest tests.python.test_persistent_checkpoints -v`. Expected: FAIL (`RewriteSaveStateImages` is not declared; `checkpoint_dir` is ignored or rejected).
- [ ] **Step 4: Implement.**
  - **`create_session`.** It parses `checkpoint_dir` with the Global Constraints errors, builds the persistent store (`CheckpointDirLocked` becomes `-32003`), and logs each `Skipped()` message as a warning. `Checkpoint` fills `DiskSource::statePath` with `ImagePathForAppleWin(workingPath)` and passes `"stepped"` or `"realtime"`.
  - **`list_checkpoints`** adds `compatible` (`entry.build == BuildId()`) to every entry.
  - **`restore_checkpoint`.** After `Get` and before anything else, a different build gets `-32011`, worded `checkpoint <id> was made by build <b>; this harness is <BuildId()>`. A `foreign` entry takes the new path:
    1. `k = ++restoreDirs_`. Copy each disk copy to `<work>/restore-<k>/disks/<workingPath filename>`.
    2. `RewriteSaveStateImages(entry.StateFile(), <work>/restore-<k>/state/state.yaml, {statePath → ImagePathForAppleWin(new path)})`.
    3. `CheckSnapshot` on the rewritten file with the new paths. Any failure up to here removes `restore-<k>` and gives `-32004` with nothing else touched.
    4. `EjectAll`, then `LoadSnapshot(rewritten, entry.meta)`. A `SnapshotPreLoadError` re-inserts the old disks, removes `restore-<k>` and gives `-32004` (the session stays usable). A failure in the load makes the session unusable, as today.
    5. On success, add each new working copy to `originals_` with its `originalPath`.
    6. Run the same purity steps as the existing restore. Move them into one helper both paths call.

    A non-foreign entry restores exactly as today.
  - **Docs.**
    - `docs/api_spec.md`: `create_session` (the parameter and its errors); `close_session` (a `checkpoint_dir` is left as it is); the Checkpoints section (persistence, ids, `.tmp`, the lock, restoring a checkpoint from an earlier process, `list_disks` after it); `list_checkpoints` (`compatible`); `restore_checkpoint` (`-32011`); the error table (`-32011`).
    - `CHANGELOG.md` Unreleased: "Harness: `create_session` takes `checkpoint_dir`; checkpoints there persist across processes and record their build; `-32011` for a checkpoint from another build (D18)."
    - `docs/specs/persistence.md`: status "§3.1–3.2 implemented".
- [ ] **Step 5: Run** the Step 3 commands. Expected: PASS. Then build Debug and Release and run all three suites in Global Constraints. Expected: PASS. Existing tests are unchanged.
- [ ] **Step 6: Commit**
  ```bash
  git add src/server/session.h src/server/session.cpp src/server/protocol.h src/host/emulator.h src/host/emulator.cpp tests/snapshot_test.cpp tests/python/test_persistent_checkpoints.py docs/api_spec.md CHANGELOG.md docs/specs/persistence.md
  git commit -m "feat: create_session takes checkpoint_dir; checkpoints restore in a later process (D18)"
  ```
