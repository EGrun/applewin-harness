# M4 Checkpoints Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Exact checkpoint/restore, a state checksum, `step_cycles`, and disk listing and export. Replay from a checkpoint is byte-identical in frames, audio and checksum.

**Architecture:**
- `Emulator` gains snapshot primitives built on AppleWin's `Snapshot_SaveState`/`Snapshot_LoadState`. They enforce restore purity: speaker, scanner, random seed and harness state are all reset deterministically.
- `Session` owns a `CheckpointStore` (directory, metadata, disk copies) and the new methods.
- **Tests come first (Task 1).**

**Tech Stack:** C++20, MSVC v143, doctest, Python 3 standard library.

**Spec:** `docs/specs/m4.md` (binding), plus `docs/specs/m3.md` for everything unchanged.

## Global Constraints
- **No AppleWin modification.** If restore purity can't be reached without a patch, stop and report.
- **Checkpoints:** ids are `cp-<n>`; at most 256; `name` up to 64 printable characters. They live at `<work>/checkpoints/<id>/state.yaml`, together with the disk copies and `meta.json`.
- **`step_cycles`:** 1–17,030,000.
- **Error codes:** `-32004` for checkpoint failures, `-32005` for an empty drive in `export_disk`, `-32602` for bad paths and ids.
- **Disks:** the original images are never opened for writing.
- **After a restore** the session is Paused.
- **Goldens unchanged:** M1, M2, and the M2 golden over the pipe.
- **Test suites:** `windowed` for display-mode comparisons; `audio` as before. CI excludes both.

## Review Focus
1. **AppleWin opens a file dialog on restore when a disk path is missing.** Expected: never. The disk copies are in place before `Snapshot_LoadState`. *(Task 1 test: restore after `delete_checkpoint` of a *different* checkpoint still works; restore after the working copy was removed by force gives `-32004`, not a hang.)*
2. **Restore isn't pure.** Expected: two restores from different prior states replay identically. *(Pass condition 1 test, with "unrelated work" that types keys, beeps the speaker and moves the disk head between restores.)*
3. **A save-state load message box blocks or is lost.** Expected: any `FrameMessageBox` during load becomes `-32004` with its text, and the session goes unusable. *(Task 1 test: corrupt `state.yaml` in place, then restore gives `-32004`.)*
4. **A checkpoint while Running catches full speed or Mockingboard mute.** Expected: the realtime run ends first. *(Task 1 test: realtime session at `"max"`, checkpoint, restore in stepped, then `get_speed.full_speed_active` is false and the checksum matches a stepped-made checkpoint at the same cycle.)*
5. **`export_disk` escapes or overwrites.** Expected:
   - a relative path, an existing file, the original image, or a path inside the work directory each give `-32602`;
   - the export contains the flushed current track, with the `SAVE`d file present.

   *(Task 1 tests.)*

---

### Task 1: Interfaces, stubs, and the failing M4 test suite
**Files:**
- **Create:**
  - `src/server/checkpoint_store.{h,cpp}`;
  - tests `tests/checkpoint_test.cpp` (in-process `ckpt:`), `tests/python/test_checkpoints.py`.
- **Modify:** `src/host/emulator.{h,cpp}` (stub declarations), `src/server/session.cpp` (method stubs returning `-32603`), and the vcxproj.

**Interfaces (Produces):**
- **`emulator.h`:**
  - `struct SnapshotMeta { uint64_t emulatedCycles; uint64_t powerOnOrigin; SpeedSettings speed; }`
  - `SnapshotMeta SaveSnapshot(const std::filesystem::path& stateFile)`, which ends any realtime run first
  - `void LoadSnapshot(const std::filesystem::path& stateFile, const SnapshotMeta&)`, which enforces restore purity and throws `std::runtime_error` carrying any `FrameMessageBox` text
  - `std::string StateChecksum() const`
  - `void RunCycles(uint64_t cycles)` (stepped)
  - `std::vector<DiskInfo> Disks() const`
  - `void FlushDisks()`
  - `void EjectAll()` and `void ReinsertAll()`, used around restore
- **`checkpoint_store.h`:**
  - `CheckpointStore(std::filesystem::path workDir)`
  - `Entry Create(std::string name, const SnapshotMeta&, const std::vector<DiskInfo>&)`, which copies the disks
  - `const Entry& Get(const std::string& id) const`
  - `std::vector<Entry> List() const`
  - `void Delete(const std::string& id)`
  - `void RestoreDisks(const Entry&, const std::vector<DiskInfo>&)`

- [ ] **Step 1: write the tests.**
  - **`ckpt:` (in-process):**
    - Pass condition 1 in-process (the restore-purity core).
    - Pass condition 6 (`PowerCycle` equals a fresh boot; compare frame hashes every 10 frames through the DOS boot against a fresh process's values recorded via the CLI).
    - Checksum stability: the same state gives the same checksum; one changed byte of RAM changes it.
    - `RunCycles` is exact (± the last instruction).
    - The restore error paths from Review Focus 1 and 3.
  - **Python `test_checkpoints.py`:**
    - Pass conditions 1, 2 and 4.
    - Review Focus 4 and 5.
    - The `list_`/`delete_checkpoint` shapes.
    - The 256-checkpoint limit. Create 256 cheaply, then the 257th gives `-32004`.
    - `input_applied { discarded: true }` for input pending at restore.
    - `get_audio` from before the restore gives `-32009`.
  - **Windowed:** pass conditions 3 and 5 in suite `windowed` (Python skips unless `HARNESS_WINDOWED=1`, and C++ has a CLI-launched version).
- [ ] **Step 2:** Build with 0 warnings. Old tests pass; new tests fail. Commit: `test: M4 failing checkpoint suite and stubs`.

### Task 2: Emulator snapshot primitives and restore purity
**Files:** `src/host/emulator.{h,cpp}`, and `src/host/harness_frame.cpp` only if the frame snapshot needs a re-render hook.
- [ ] **Step 1:** Implement them.
  - **Save:** `Snapshot_SetFilename` and `Snapshot_SaveState`. Check `FrameMessageBox` before and after.
  - **Load:** `Snapshot_LoadState`, then the purity steps in the spec's order.
  - **Investigate and fix:** what AppleWin's load leaves stale (speaker, scanner, card timers, Mockingboard voice offsets). Keep a written list in the report.
  - **`StateChecksum`:** the spec's layout, with MemSaveSnapshot-style flushing.
  - **`PowerCycle`:** the scanner/capture reset (pass condition 6).
- [ ] **Step 2:** Run `-tc="ckpt:*"`, the full suite, and the goldens. Commit: `feat(host): pure snapshot restore, state checksum, RunCycles`.

### Task 3: CheckpointStore, disks, and the session methods
**Files:** `src/server/checkpoint_store.cpp`, `src/server/session.cpp`.
- [ ] **Step 1:** Implement the store: directory per checkpoint, `meta.json` via nlohmann, copying disks with `WorkingCopy`-style errors.
- [ ] **Step 2:** Implement every method in the spec's API section, including the purity hooks the session owns: clearing the input queue with discarded notifications, resetting detectors, the unusable state after a load failure.
- [ ] **Step 3:** Run the Python suite in Debug and Release, then the full C++ suite. Commit: `feat(server): checkpoints, state checksum, step_cycles, disks`.

### Task 4: Display-mode and speech determinism
**Files:** tests only, plus fixes wherever they turn out to be needed.
- [ ] Make pass conditions 3 and 5 green.
  - Find the SSI263 on AppleWin's default slot-4 card (Mockingboard C / Phasor). Write a BASIC POKE program that speaks a few phonemes, and verify the capture holds speech: SSI263 voice commits, with non-silent audio.
  - If the default config has no SSI263, report it, and the controller rules.
- [ ] Run `-ts=windowed` and the Python windowed tests. Commit: `test: display-mode and SSI263 determinism`.

### Task 5: Docs
**Files:** `docs/api_spec.md` (checkpoints, checksum layout, `step_cycles`, disks, the `input_applied.discarded` field), `docs/decisions.md` (D5 update: restore purity), `project.md` (M4 status; parked items resolved or carried), `README.md`.
- [ ] Update them. Mark M4 ✅ only if every pass condition is green. Commit: `docs: M4 checkpoints`.
