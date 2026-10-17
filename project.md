# AppleWin AI Harness — Project Plan

## Goal
Let an AI agent use an emulated Apple II the way a person does: type on the keyboard, move the joystick, look at the screen, and listen to its sound. The emulator is AppleWin. Experiments must be safe (original disk images are never modified) and reproducible (runs can be stepped, checkpointed and compared).

## Scope

**In scope**
- Two display modes: windowed by default, so a person can watch the Apple screen and hear its sound (speaker, Mockingboard), and headless (`--headless`), with no window or sound, for CI and batch runs (D8).
- Keyboard and joystick input.
- Sound for the agent: raw audio and start/stop events, captured in emulated time in every mode (D12).
- Screen observation.
- Disk images through harness-managed working copies.
- Pause, step and checkpoint/restore for deterministic runs.
- A local JSON-RPC API.
- A reference agent adapter.

**Out of scope for now**
- Remote/network access.
- Platforms other than Windows.
- Sector-level disk editing.
- Session record/replay.

## Architecture
See `docs/decisions.md` for the reasoning behind each choice.

```
 agent / MCP adapter          test runner / operator
          \                      /
       JSON-RPC 2.0 over named pipe  (+ optional shared-memory frames)
                      |
        AppleWinHarness.exe ── owns the main loop
          ├─ headless FrameBase implementation
          ├─ presenter: view-only window + DirectSound (default) | none (--headless)
          ├─ input:   KeybQueueKeypress / JoySetPosition / JoySetButton
          ├─ frames:  Video::GetFrameBuffer (BGRA32)
          ├─ audio:   capture tap (speaker + Mockingboard) → ring buffer + sound events
          ├─ control: CpuExecute per frame; pause / step
          └─ sessions: working disk copies + checkpoints (save state + disks)
                      |
           AppleWin core (linked in-process; submodule external/AppleWin)
```

- Integration: the harness hosts the AppleWin core in-process (D2, confirmed in M1).
- API: split into an agent API and an operator API (D4). The spec is in `docs/api_spec.md`.
- State: checkpoints cover machine state and disks together, and a restore leaves the machine and the harness depending only on the checkpoint (D5).
- Display and sound: windowed with sound by default, headless (no window or sound) as an option. The window and sound are output only; they never change emulated state (D8).
- Pacing: realtime mode runs 1 ms bursts of cycles paced by a high-resolution timer, plus audio-buffer feedback from the harness's own `AudioOutput` when sound is playing. Stepped mode runs exact cycle counts with no timer and no sound (D9).
- Time: all API durations are in emulated milliseconds, and every input reports the cycle it was applied at (D10).
- Agent audio: captured from virtual sound buffers (D14), in emulated time, in every mode. Exposed as `get_audio` and as `sound_started`/`sound_stopped` events (D12).
- Speed: the operator API mirrors AppleWin's speed controls exactly: the speed slider, clock multiplier, enhanced disk speed, the Scroll Lock full-speed override, and the 50/60 Hz setting (D11).

## Key AppleWin files
All paths are under `external\AppleWin\source\`.

- **Frontend seam:** `FrameBase.h`, `Interface.h`, `Windows\Win32Frame.cpp` (reference implementation)
- **Main loop:** `Windows\AppleWin.cpp` (`ContinueExecution`), `CPU.cpp` (`CpuExecute`), `Core.cpp`, `SynchronousEventManager.cpp`
- **Video:** `Video.cpp/.h`, `NTSC.cpp/.h`, `VidHD.cpp/.h`
- **Sound:** `SoundBuffer.h`, `SoundCore.cpp`, `Speaker.cpp`, `Mockingboard.cpp`, `MockingboardCardManager.cpp`, `Riff.cpp` (WAV output, the model for the capture tap), `Windows\DXSoundBuffer.cpp`
- **Input:** `Keyboard.cpp`, `Joystick.cpp`, `FourPlay.cpp`
- **Disks:** `Disk.cpp`, `DiskImage.cpp`, `Harddisk.cpp`
- **State and config:** `SaveState.cpp`, `CmdLine.cpp`, `Registry.cpp`

## Milestones
Each milestone ends with a concrete acceptance check. Security and tests are built in as we go, not left for the end.

### M0 — Repository and build ✅
VS 2022 solution, `.gitignore`, README, decisions log.

**Acceptance:** `AppleWinHarness.exe` builds in both Debug and Release.

### M1 — Spike: host the AppleWin core headless ✅
Link the AppleWin core into the harness. Implement a minimal headless `FrameBase`. Drive `CpuExecute` from the harness loop at authentic speed, paced by the realtime loop (D9). Write a frame to disk as a PNG.

**Acceptance:** the harness boots an Apple //e with no window and saves a PNG showing the boot screen. The spike also produces a list of the AppleWin patches it needed, which feeds the D2 decision.

**Patch list:** empty. The core links unchanged and no file under `external/AppleWin` is modified.

**Exit criteria:**
- If the core can't be separated from Win32 with a small patch set, revisit D2 (fallback: plugin inside AppleWin.exe).
- Resolve D6 (licensing) before publishing.

### M2 — Vertical slice, in-process ✅
Add keyboard input and frame capture. Add the windowed presenter with sound, which is the default, and the `--headless` option (D8). Mount a DOS 3.3 disk as a working copy. Add a C++ test and CI.

**Patch list:** still empty. M2 needed no AppleWin patches.

**Acceptance:**
- An automated test boots DOS 3.3 and types `PRINT 2+2⏎`. The captured frame matches a golden checksum.
- The test passes in both windowed and headless mode with the same checksum.
- CI runs it with `--headless` on a Windows runner. The runner checks out submodules (D7).
- Running the harness with no options shows a live window and plays sound. Typing into that window has no effect on the emulator.
- In windowed realtime mode, BASIC `PRINT CHR$(7)` plays the bell. A 60-second speaker test at authentic speed produces no buffer underruns.
- Headless mode opens no audio device.

### M3 — JSON-RPC server ✅
**Status:** Done: Parts A (server), B (agent audio) and C (speed controls). See `docs/specs/m3.md`.

Serve the named pipe with an ACL and the `hello` handshake. Implement the agent API (keys, joystick, `get_frame`, `subscribe_frames`) and the operator API for `create_session`, `close_session`, `pause`, `resume` and `step_frames`. `create_session` and `close_session` moved here from M4, because the M2 scenario over the pipe needs them. There is one session per process; multiple sessions need AppleWin re-initialisation work and come later.

This milestone also adds `set_speed` and `get_speed` (D11), and agent audio: virtual sound buffers (D14), `get_audio` and `subscribe_audio` (D12).

**Acceptance:**
- A small Python client reproduces the M2 test over the pipe.
- A connection from another user account, or with a bad token, is rejected.
- In realtime mode, measured emulated cycles per wall-clock second are within ±2% of `effective_clock_hz` for `machine_speed` values 5, 10 and 20.
- `"max"` and `full_speed_override` both run well above 2× speed.
- For a range of `clock_multiplier` inputs, the resulting `machine_speed` matches what AppleWin's `UseClockMultiplier` produces.
- Agent audio (Part B, done): in headless mode, `PRINT CHR$(7)` produces a `sound_started` event followed by a `sound_stopped` event with a `dominant_hz` close to the bell's pitch (about 935 Hz). `get_audio` returns the bell, and the same stepped script gives byte-identical audio in both display modes.
- Part C (done): `set_speed`/`get_speed`, `create_session.speed`, full speed with AppleWin's redraw cadence and a `frame_ready` cap of one per 16 ms.
- The M2 human bell check still stands (a person listening to the windowed bell).
- Parked for M4/M6: resampled windowed playback at non-authentic speeds, and PAL (`video_refresh: 50`).

### M4 — Checkpoints, determinism ✅
**Status:** Done. See `docs/specs/m4.md` and the checkpoint section of `docs/api_spec.md`. No AppleWin patches were needed.

Adds `checkpoint`, `restore_checkpoint`, `list_checkpoints`, `delete_checkpoint`, `get_state_checksum`, `step_cycles`, `list_disks` and `export_disk`. (`create_session`, `close_session` and stepped run mode arrive in M3.) Restore purity (D5) resets every piece of machine and harness state that AppleWin's save state doesn't cover.

**Acceptance (all pass, headless and windowed):**
- The same input script, run twice from the same checkpoint in stepped mode, produces identical state checksums and frame checksums (ruling D4: both scripted runs start from a restore).
- The checksums are also identical when the script runs at `machine_speed` 10 and at 40.
- `get_audio` over the same cycle range returns byte-identical audio across those runs, and in both windowed and headless mode.
- The original disk image's hash is unchanged; a `SAVE` is exported with `export_disk`, and a restore to before it brings back the disk without the file.
- SSI263 speech (Mockingboard, slot 4) is covered by an end-to-end determinism test that runs a BASIC program in both display modes and across restores (D14).
- Frames after an in-process `PowerCycle` equal a fresh `Start`'s; the M1 and M2 goldens still pass.

**Carried from the M3 Part C final review** (M4-1, speed in stepped runs, is resolved by ruling C7):
- M4-2 resolved: the session's speed settings are re-applied after a restore (D8).
- M4-3 resolved: `srand(1)` again on restore (D13).
- M4-4 resolved: the input queue, held keys, audio capture, sound detectors and cycle origin are cleared or restored (D5).
- M4-5 resolved: the speaker internals, Mockingboard frame counter, clock, FLASH counter and the video scanner phase (also after `PowerCycle`) are reset (D5).
- M4-6 resolved: checkpoints and replays key on `emulated_cycles`.
- M4-7 resolved: a checkpoint ends any realtime run first.

**Parked:**
- Split-independent audio capture: replays must reuse the same step partition (ruling B13). Anchoring each voice by its own cumulative sample count is the planned fix.
- D10 mitigation: checkpoints refuse non-ASCII working paths; inserting working copies through ASCII 8.3 short paths is parked to M6.
- Minor findings from the M4 final review will be tracked separately.

### M5 — Agent adapter
Build an MCP server that wraps the agent API, and an example agent task (for example, write and run a short BASIC program).

**Acceptance:** an MCP-capable agent such as Claude completes the example task end to end.

**Status:** implemented (the `adapter/applewin_mcp` MCP server, the harness `get_text_screen`, the example task `adapter/examples/basic_count.md` and CI), awaiting human acceptance: a person points Claude Code or Claude Desktop at the server and checks that the agent completes the example task (spec: `docs/specs/m5.md`).

### M6 — Hardening and release
Add input rate limits, request size limits, structured logging, and a portable release bundle. Run the end-to-end scenario in CI.

### Later (optional)
Session record/replay, batch experiment orchestration, sector-level disk access.

## Risks
- **The core may be coupled to Win32** (DirectSound, DirectInput, registry, the window message loop). Mitigation: the M1 spike, and the D2 fallback.
- **Maintaining a patch set against AppleWin upstream.** Mitigation: keep patches minimal and apply them on top of the pinned submodule commit (D7).
- **GPL-2.0 obligations** (D6).
- **Hidden sources of non-determinism**, such as the real-time clock, the no-slot clock, random memory initialisation, and AppleWin's speaker feedback (which changes how many cycles each burst runs, depending on the host audio buffer). Mitigation: speaker feedback only runs in windowed realtime mode, stepped mode has no feedback and pins or disables the rest (D9), and M4 tests for it.
- **The replacement DXSoundBuffer TU must track AppleWin's interface** on submodule updates (D14). Mitigation: it is one small file, and the audio tests fail loudly if the interface drifts.
- **Realtime input timing depends on when clients send input.** Mitigation: every input reports `applied_at_cycle` (D10). Use stepped mode when a run must be reproducible.
- **Frame timing:** whole-frame capture at vsync may miss mid-frame effects. Accepted for now; per-scanline capture isn't planned.

## Open questions
- Should windowed mode optionally let a person type into the window to take over from the agent? It's view-only for now (D8). Allowing input would need a way to take and hand back control, and runs with human input wouldn't be deterministic.
- Should the agent get the derived text-screen observation (`get_text_screen`) by default? It's a big help to LLM agents, but strictly goes beyond pixels. Currently it's an option on the session (see the API spec).
- Which machine models must be supported? The default is Apple //e Enhanced; others are available through `-model`.
