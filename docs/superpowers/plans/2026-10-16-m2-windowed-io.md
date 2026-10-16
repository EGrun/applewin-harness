# M2 Windowed I/O Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot DOS 3.3 from a working copy, type `PRINT 2+2` on the emulated keyboard, and get the same frame checksum windowed (with sound) and headless. Also add CI.

**Architecture:**
- One `FrameBase`, renamed `HarnessFrame`, serves both display modes (D8).
- Windowed mode adds two things: a view-only `Window`, fed by a present hook, and DirectSound buffers wrapped in `MonitoredSoundBuffer`, which handles mute and counts underruns.
- The `Emulator` run loops gain `Pacing` (stepped or realtime with speaker feedback) and `TypeText`.
- The CLI becomes an ordered list of steps.
- **The whole new test suite is written first (Task 2), against stub bodies.** Tasks 3 to 9 turn it green.

**Tech Stack:** C++20, MSVC v143, MSBuild, doctest 2.4.11, Win32 GDI, DirectSound (AppleWin's `DXSoundBuffer`), GitHub Actions.

**Spec:** `docs/specs/m2.md`. Read it with this plan; the interfaces, startup order and CLI contract there are binding. `docs/specs/m1.md` still holds for everything M2 doesn't change.

## Global Constraints
- x64 only, Debug and Release. v143. Harness code is C++20 at warning level 4, with 0 warnings.
- **No file under `external/AppleWin` may be modified.** If a patch seems unavoidable, stop and report it.
- No new dependencies.
- Nothing may show modal UI or write to `HKCU\Software\AppleWin`.
- Golden files: `tests/golden/m1_boot_120.sha256` (unchanged) and `tests/golden/m2_dos_print.sha256` (new, provisional until a person approves it).
- DOS image: `external/AppleWin/bin/DOS 3.3 System Master - 680-0210-A.dsk`. Tests locate it from the test exe at `<exe>/../../../external/AppleWin/bin/...`.
- Cycles per frame: 17,030. Typing timeout: 600 frames. Window client area: 1120×768. Window class: `AppleWinHarnessWindow`. Window title: `AppleWin Harness (view only)`.
- Doctest suites: `realtime` (M1), `windowed` (needs a desktop), `audio` (needs an audio device). CI runs `-tse=windowed,audio`.
- Tests and helpers must not leave files in `%TEMP%\applewin-harness` beyond the M1 `tests.ini`.

## Review Focus
1. **Sound buffer left looping after a run.** When a realtime run ends, the DirectSound buffer keeps replaying its contents, which buzzes. Expected: muted outside realtime runs. *(Task 2 test: after `SetMuted(true)` the inner volume is `DSBVOLUME_MIN`, and AppleWin's `SetVolume` while muted does not reach the inner buffer.)*
2. **Spurious underruns after a mute period.** The play cursor laps the stale write offset while muted. Expected: no underrun is counted until the first commit after unmuting. *(Task 2 test.)*
3. **A key queued before DOS reads the keyboard is lost, or typed twice.** Expected: each byte is queued only once the strobe is clear, and the screen shows exactly `]PRINT 2+2`. *(Task 2 in-process test checks the exact rows.)*
4. **A run that writes the disk modifies the original image.** Expected: the original's hash and mtime are unchanged, and the copy is gone afterwards. *(Task 2 test: `WorkingCopy` round trip, plus the CLI test checks the original after a run.)*
5. **The window closed mid-script hangs the harness or leaks the working copy.** Expected: exit 1 and `error: window closed`, with the copy removed. *(Task 2 `windowed`-suite test: post `WM_CLOSE` during a long realtime run, then check the exit code and that `%TEMP%\applewin-harness\<pid>` is gone.)*

**Build command** (every task):
```bash
MSB=$("/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | head -1)
"$MSB" AppleWinHarness.sln -m -nologo -v:minimal -p:Configuration=Debug -p:Platform=x64
```
**Test command:** `bin/x64/Debug/HarnessTests.exe` (all), `-ts=<suite>` for one suite, `-tc="<name>"` for one test.

---

### Task 1: Build hygiene and the HarnessFrame rename (no behaviour change)

**Files:**
- Create: `build/Harness.props`
- Modify: `build/HarnessCore.vcxproj`, `AppleWinHarness.vcxproj`, `tests/HarnessTests.vcxproj`, `src/host/emulator.h`, `src/host/emulator.cpp`, `src/host/applewin_host.cpp`, `src/main.cpp`
- Rename: `src/host/headless_frame.{h,cpp}` → `src/host/harness_frame.{h,cpp}`, `tests/headless_frame_test.cpp` → `tests/harness_frame_test.cpp`

**Interfaces:**
- Produces: `harness::HarnessFrame` (the same members as `HeadlessFrame`). The test prefix becomes `frame:`. `emulator.h` includes only the standard library and `core/frame_view.h`. `EmulatorConfig` loses `model`.

- [ ] **Step 1:** Move the repeated include directories, defines, `AdditionalDependencies` and output settings into `build/Harness.props`, and import it in the three projects. Add `delayimp.lib` and `<DelayLoadDLLs>dsound.dll</DelayLoadDLLs>` for both exes.
- [ ] **Step 2:** Do the rename. Strip the AppleWin includes from `emulator.h`; `emulator.cpp` hardcodes `A2TYPE_APPLE2EENHANCED`. Add `catch (...)` to `main.cpp`'s `Run`, exiting with 1 and `error: unknown exception`.
- [ ] **Step 3:** Build Debug and Release. Expected: 0 warnings, and all 30 M1 tests pass in both.
- [ ] **Step 4:** Commit: `refactor: shared props, delay-load dsound, HarnessFrame rename, header hygiene`.

### Task 2: Interfaces, stubs, and the full failing M2 test suite (spec-driven)

The implementer writes tests from `docs/specs/m2.md` only.

**Files:**
- Create: `src/host/monitored_sound_buffer.{h,cpp}`, `src/host/window.{h,cpp}`, `src/core/working_copy.{h,cpp}` (headers complete, bodies stubbed to throw `std::logic_error("not implemented")` or return a default), `tests/monitored_sound_buffer_test.cpp`, `tests/window_test.cpp`, `tests/working_copy_test.cpp`, `tests/dos_test.cpp`, `tests/support/fake_sound_buffer.h`, `tests/support/paths.h`
- Modify: `src/host/emulator.h`/`.cpp` (new declarations, stub bodies), `src/host/harness_frame.h`/`.cpp` (new members, stubs), `tests/harness_frame_test.cpp`, `tests/emulator_test.cpp`, `tests/cli_test.cpp`, the vcxproj files

**Interfaces:**
- Produces: every declaration in the spec's "Harness modules" section, exactly as written there.
  - `tests/support/paths.h`: `RepoRoot()`, `DosMasterImage()`.
  - `tests/support/fake_sound_buffer.h`: a `FakeSoundBuffer : SoundBuffer` with settable play/write cursors and recorded `SetVolume` values.
  - `cli_test.cpp`: the helpers `RunCli(args)`, `LaunchCli(args)` (non-blocking, returns process info) and `FindHarnessWindow(pid)`.

- [ ] **Step 1: Write the tests.** One `TEST_CASE` per bullet, with the spec's exact values.
  - `frame:` (harness_frame_test):
    - `PresentedFrame` is all zero before the first present.
    - After a pattern is written to the video buffer and `VideoPresentScreen()` is called, `PresentedFrame` shows the pattern, and it is unchanged after the live buffer is overwritten.
    - The present hook is called once per present.
    - With sound disabled, `CreateSoundBuffer` returns null and `SoundBuffersCreated()==0`.
  - `sound:` (monitored_sound_buffer_test, using `FakeSoundBuffer` with `bufferBytes=4096`):
    - Calls are forwarded.
    - Mute sets `DSBVOLUME_MIN` on the inner buffer.
    - `SetVolume(-500)` while muted is not forwarded, and `GetVolume` returns -500.
    - Unmute restores -500.
    - With play=100, write=200: a commit at offset 0 of 150 bytes, then `GetCurrentPosition` → `Underruns()==1`.
    - The same commit ending at 250 → 0.
    - The wrap-around zone (play=4000, write=100) with the commit end at 50 → 1.
    - After an underrun, a second `GetCurrentPosition` doesn't count again until the next commit.
    - A whole-buffer (4096-byte) commit stops tracking.
    - No count while muted, or after unmuting until the next commit.
  - `window:` (window_test):
    - Construction gives a visible window with class `AppleWinHarnessWindow` and a 1120×768 client area.
    - `Show` of a 2×2 bottom-up test image gives `Pixels()` top row first.
    - `WM_CHAR` / `WM_KEYDOWN` / `WM_LBUTTONDOWN` sent to it leave `Pixels()` unchanged and the window open.
    - `WM_CLOSE` then `Pump()` returns false and `Closed()` is true.
  - `copy:` (working_copy_test, using temp files):
    - The copy exists with the same bytes and a cleared read-only attribute.
    - The original's bytes and mtime are unchanged.
    - The destructor removes the copy and the empty work dir.
    - A missing original throws `std::runtime_error` containing the file name.
    - A directory passed as original throws.
  - `emulator:` additions (emulator_test):
    - `EmulatedCycles()==0` straight after `PowerCycle()`.
    - `TypeText("\x01")` throws `std::invalid_argument`, and the keyboard is untouched.
    - `InsertDisk(2, …)` and `EjectDisk(-1)` throw `std::out_of_range`.
    - `InsertDisk(0, "<missing>.dsk")` throws `std::runtime_error`.
    - After `Start` in headless mode, `SoundActive()==false`, `GetModuleHandleW(L"dsound.dll")==nullptr` and `SoundBuffersCreated()==0`.
    - `RunFrames(5)` returns true.
  - `dos:` (dos_test):
    - `InsertDisk(0, DosMasterImage())`, `PowerCycle()`, `RunFrames(720)`, `TypeText("PRINT 2+2\r")`, `RunFrames(60)`. Text page 1 then has consecutive rows that trim to `]PRINT 2+2`, `4`, `` (empty; DOS 3.3 prints a blank line), `]`.
    - `FrameHash(Frame())` equals the golden `m2_dos_print.sha256`.
    - A scope guard calls `EjectDisk(0)` and `PowerCycle()`, so the M1 boot tests stay valid.
  - `cli:` (cli_test):
    - M1's tests are switched to `--headless --frames 120`.
    - The headless golden (pass condition 1): stdout matches exactly `sha256 <golden>\r?\n`, the PNG is 560×384, and the original image's SHA-256 and mtime are unchanged.
    - Usage errors exit 2: headless with no steps; `--type ""`; `--type "a\qb"`; `--frames abc`; `--disk1` given twice; `--boot-frames 120`.
    - A missing `--disk1` file exits 1.
    - After a run, `%TEMP%\applewin-harness\<pid>` does not exist.
  - `cli:` suite `windowed`:
    - The windowed golden prints the same hash; the first stdout line is the hash line.
    - No arguments: within 10 s a visible `AppleWinHarnessWindow` owned by the child appears, and `WM_CLOSE` exits it with 0.
    - View-only: `--realtime --disk1 <DOS> --frames 720`, with `HELLO\r` posted as `WM_CHAR`s (plus `WM_KEYDOWN`/`WM_LBUTTONDOWN`) from 3 s to 6 s, gives the same hash as `--headless --disk1 <DOS> --frames 720`.
    - `WM_CLOSE` during `--realtime --disk1 <DOS> --frames 3000` exits 1, with `error: window closed` on stderr and the work dir removed.
  - `cli:` suite `audio`: the 60-second tone run reports `audio underruns 0`. It is skipped if the warning `no audio device` appears.
- [ ] **Step 2:** Build. Expected: 0 errors and 0 warnings. The M1 tests that don't touch changed contracts pass. Every new test fails or throws `not implemented`. Record the failing list in the report.
- [ ] **Step 3:** Commit: `test: M2 failing test suite and interface stubs`.

### Task 3: Presented-frame snapshot and cycles since power-on

**Files:** `src/host/harness_frame.{h,cpp}`, `src/host/emulator.cpp`
**Interfaces:** Consumes the Task 2 declarations. Produces `Emulator::Frame()` backed by `HarnessFrame::PresentedFrame`.

- [ ] **Step 1:** Implement the snapshot and the hook in `VideoPresentScreen`. Back `Frame()` with the snapshot. Record `g_nCumulativeCycles` at `Start`/`PowerCycle` for `EmulatedCycles()`.
- [ ] **Step 2:** Run `-tc="frame:*,emulator:*"`. Expected: the `frame:` tests pass; the M1 golden boot tests still pass (the boot screen is static); the power-on cycles test passes.
- [ ] **Step 3:** Commit: `feat(host): frames are captured at present; cycles count from power-on`.

### Task 4: Working copy, disk insert/eject, joysticks off

**Files:** `src/core/working_copy.cpp`, `src/host/emulator.cpp`
**Interfaces:** Produces `WorkingCopy`, `Emulator::InsertDisk/EjectDisk`, and `EmulatorConfig::disk1` handling.

- [ ] **Step 1:** Implement `WorkingCopy` with `std::filesystem::copy_file` and `permissions` (clear read-only).
- [ ] **Step 2:** Implement `InsertDisk` through `Disk2InterfaceCard::InsertDisk(drive, path, false, false)`. A non-`eIMAGE_ERROR_NONE` result throws with the message captured by `FrameMessageBox` (or the error code if there is none). `EjectDisk` goes through `Disk2InterfaceCard::EjectDisk`. In `Start`, call `JoySetJoyType(JN_JOYSTICK0, J0C_DISABLED)` and `JoySetJoyType(JN_JOYSTICK1, J1C_DISABLED)` before `JoyInitialize()`, and insert `disk1` before `Boot()`.
- [ ] **Step 3:** Run `-tc="copy:*,emulator:*"`. Expected: these pass.
- [ ] **Step 4:** Commit: `feat: disk working copies and insert/eject; host joysticks never read`.

### Task 5: Pacing, speaker feedback, TypeText, DOS golden

**Files:** `src/host/emulator.cpp`, `tests/golden/m2_dos_print.sha256`
**Interfaces:** Produces `RunFrames(frames, Pacing)`, `RunRealtime` with feedback, and `TypeText`. All three return false when `Window` is closed; until Task 9 there's never a window, so they return true.

- [ ] **Step 1:** Factor one private step function (≤1 ms, paced or not), used by `RunFrames`, `RunRealtime` and `TypeText`. Realtime burst: `max(0, budget.Next() + g_nCpuCyclesFeedback)`, capped at the cycles remaining. `TypeText` validates every byte first, then waits for the strobe as the spec describes.
- [ ] **Step 2:** Run `-tc="dos:*"` without a golden. Expected: the rows test passes. If 600 frames isn't enough for DOS to reach `]`, rule on a larger value, change it in the spec, the plan and the tests together, and ledger the ruling.
- [ ] **Step 3:** Write the observed hash to `tests/golden/m2_dos_print.sha256`, and save the PNG to `<scratchpad>/m2_dos_print.png` for human review. Run all non-CLI tests. Expected: pass, including the `realtime` suite.
- [ ] **Step 4:** Commit: `feat(host): paced runs with speaker feedback, TypeText; provisional DOS golden`.

### Task 6: MonitoredSoundBuffer and HarnessFrame sound

**Files:** `src/host/monitored_sound_buffer.cpp`, `src/host/harness_frame.cpp`
**Interfaces:** Produces the spec's `MonitoredSoundBuffer` and the `HarnessFrame::EnableSound/SetSoundMuted/SoundUnderruns/SoundBuffersCreated` members. With sound on, `CreateSoundBuffer` calls `DXSoundBuffer::create(...)` (include `Windows/DXSoundBuffer.h`) and wraps the result.

- [ ] **Step 1:** Implement as the spec says. For the unsafe-zone predicate, mirror `SoundCore_ValidateAndAlignWriteOffset`: strict `>` and `<` in both branches.
- [ ] **Step 2:** Run `-tc="sound:*,frame:*,emulator:*"`. Expected: pass, including the headless no-dsound test.
- [ ] **Step 3:** Commit: `feat(host): monitored DirectSound buffers with mute and underrun count`.

### Task 7: Window

**Files:** `src/host/window.cpp`
**Interfaces:** Produces the spec's `Window`.

- [ ] **Step 1:** Implement it. Register the class once (`std::once_flag`). Size the window with `AdjustWindowRectEx` for a 1120×768 client area. Store `this` in `GWLP_USERDATA` at `WM_NCCREATE`. Paint with a top-down `BITMAPINFO` (negative `biHeight`).
- [ ] **Step 2:** Run `-tc="window:*"`. Expected: pass.
- [ ] **Step 3:** Commit: `feat(host): view-only presenter window`.

### Task 8: Command line (headless behaviour)

**Files:** `src/main.cpp`
**Interfaces:** Consumes `WorkingCopy`, `Emulator::RunFrames/TypeText/SoundActive/SoundUnderruns`. Produces the spec's CLI.

- [ ] **Step 1:** Parse into `struct Args { bool headless, realtime; std::optional<path> disk1, png; std::vector<Step> steps; }`, where `Step` is `{ enum Kind { Frames, Type } kind; uint32_t frames; std::string text; }`. Decode the escapes at parse time. Keep the INI and the `WorkingCopy` in RAII objects so every exit path cleans up.
- [ ] **Step 2:** Run `-tc="cli:*" -tse=windowed,audio`. Expected: pass.
- [ ] **Step 3:** Commit: `feat: M2 command line (steps, --type, --disk1, --headless, --realtime)`.

### Task 9: Windowed mode with sound

**Files:** `src/host/emulator.cpp` (plus `harness_frame.cpp` only if the hook wiring needs it)
**Interfaces:** Consumes `Window`, `HarnessFrame` sound and the run loops. Produces the spec's windowed `Start`/`Shutdown`, the mute policy, and the window-closed rule.

- [ ] **Step 1:** Implement the spec's windowed startup order: window → `g_hFrameWindow` → `DSInit()` → (on success) `g_bDisableDirectSound=false`, `EnableSound(true)`, `SetSoundMuted(true)` → the rest of M1's startup. The present hook calls `window->Show(PresentedFrame(false))` then `window->Pump()`. The run loops also call `Pump()` once per step when windowed. Shutdown order: `SpkrDestroy()`, `GetCardMgr().GetMockingboardCardMgr().Destroy()`, `DSUninit()`, `Window` destroyed, `g_hFrameWindow = nullptr`. Before writing, grep `external/AppleWin/source` for `g_hFrameWindow` uses reachable from the core and list them in the report; none may show UI.
- [ ] **Step 2:** Run `bin/x64/Debug/HarnessTests.exe -ts=windowed`, then `-ts=audio`, then everything. Expected: all pass. The audio test passes or is skipped with the no-device message, and the report says which.
- [ ] **Step 3:** Commit: `feat: windowed mode with view-only window and DirectSound`.

### Task 10: CI and docs

**Files:** Create `.github/workflows/ci.yml`. Modify `README.md`, `project.md`, `docs/decisions.md` (the D8 "How it works" text names `HarnessFrame`), and `docs/specs/m2.md` (status).

- [ ] **Step 1:** Write the workflow:
  - Triggers: push and pull_request.
  - Runner: `windows-2022`.
  - Checkout: `actions/checkout@v4` with `submodules: recursive`.
  - MSBuild: `microsoft/setup-msbuild@v2`.
  - A matrix over Debug and Release, each building `AppleWinHarness.sln` with `-p:Platform=x64` and running `bin/x64/${{ matrix.config }}/HarnessTests.exe -tse=windowed,audio`.
  - On failure, upload `%TEMP%\applewin-harness` as an artifact.
- [ ] **Step 2:** Update the README usage section with the new CLI, including the bell example: `AppleWinHarness.exe --realtime --disk1 "<DOS>" --frames 720 --type "PRINT CHR$(7)\r" --frames 120`. Mark M2 ✅ in `project.md`, but only after the human checks. Validate the workflow YAML parses (`python -c "import yaml,sys; yaml.safe_load(open(sys.argv[1]))" .github/workflows/ci.yml`, or a careful review if PyYAML is unavailable).
- [ ] **Step 3:** Commit: `ci: GitHub Actions build and headless tests; M2 docs`.
