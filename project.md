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

- Integration: the harness hosts the AppleWin core in-process (D2, to be confirmed in M1).
- API: split into an agent API and an operator API (D4). The spec is in `docs/api_spec.md`.
- State: checkpoints cover machine state and disks together (D5).
- Display and sound: windowed with sound by default, headless (no window or sound) as an option. The window and sound are output only; they never change emulated state (D8).
- Pacing: realtime mode runs 1 ms bursts of cycles paced by a high-resolution timer, plus AppleWin's audio-buffer feedback when sound is playing. Stepped mode runs exact cycle counts with no timer and no sound (D9).
- Time: all API durations are in emulated milliseconds, and every input reports the cycle it was applied at (D10).
- Agent audio: captured at the source, in emulated time, in every mode. Exposed as `get_audio` and as `sound_started`/`sound_stopped` events (D12).
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

### M1 — Spike: host the AppleWin core headless
Link the AppleWin core into the harness. Implement a minimal headless `FrameBase`. Drive `CpuExecute` from the harness loop at authentic speed, paced by the realtime loop (D9). Write a frame to disk as a PNG.

**Acceptance:** the harness boots an Apple //e with no window and saves a PNG showing the boot screen. The spike also produces a list of the AppleWin patches it needed, which feeds the D2 decision.

**Exit criteria:**
- If the core can't be separated from Win32 with a small patch set, revisit D2 (fallback: plugin inside AppleWin.exe).
- Resolve D6 (licensing) before publishing.

### M2 — Vertical slice, in-process
Add keyboard input and frame capture. Add the windowed presenter with sound, which is the default, and the `--headless` option (D8). Mount a DOS 3.3 disk as a working copy. Add a C++ test and CI.

**Acceptance:**
- An automated test boots DOS 3.3 and types `PRINT 2+2⏎`. The captured frame matches a golden checksum.
- The test passes in both windowed and headless mode with the same checksum.
- CI runs it with `--headless` on a Windows runner. The runner checks out submodules (D7).
- Running the harness with no options shows a live window and plays sound. Typing into that window has no effect on the emulator.
- In windowed realtime mode, BASIC `PRINT CHR$(7)` plays the bell. A 60-second speaker test at authentic speed produces no buffer underruns.
- Headless mode opens no audio device.

### M3 — JSON-RPC server
Serve the named pipe with an ACL and the `hello` handshake. Implement the agent API (keys, joystick, `get_frame`, `subscribe_frames`) and the operator API for `pause`, `resume` and `step_frames`.

This milestone also adds `set_speed` and `get_speed` (D11), and agent audio: the AppleWin capture patch, `get_audio` and `subscribe_audio` (D12).

**Acceptance:**
- A small Python client reproduces the M2 test over the pipe.
- A connection from another user account, or with a bad token, is rejected.
- In realtime mode, measured emulated cycles per wall-clock second are within ±2% of `effective_clock_hz` for `machine_speed` values 5, 10 and 20.
- `"max"` and `full_speed_override` both run well above 2× speed.
- For a range of `clock_multiplier` inputs, the resulting `machine_speed` matches what AppleWin's `UseClockMultiplier` produces.
- In headless mode, `PRINT CHR$(7)` produces a `sound_started` event followed by a `sound_stopped` event with a `dominant_hz` close to the bell's pitch. `get_audio` returns the bell.

### M4 — Sessions, checkpoints, determinism
Add `create_session`, `checkpoint`, `restore_checkpoint` and `get_state_checksum`. Add stepped run mode.

**Acceptance:**
- The same input script, run twice from the same checkpoint in stepped mode, produces identical state checksums and frame checksums.
- The checksums are also identical when the script runs at `machine_speed` 10 and at 40.
- `get_audio` over the same cycle range returns byte-identical audio across those runs, and in both windowed and headless mode.
- The original disk image's hash is unchanged.

### M5 — Agent adapter
Build an MCP server that wraps the agent API, and an example agent task (for example, write and run a short BASIC program).

**Acceptance:** an MCP-capable agent such as Claude completes the example task end to end.

### M6 — Hardening and release
Add input rate limits, request size limits, structured logging, and a portable release bundle. Run the end-to-end scenario in CI.

### Later (optional)
Session record/replay, batch experiment orchestration, sector-level disk access.

## Risks
- **The core may be coupled to Win32** (DirectSound, DirectInput, registry, the window message loop). Mitigation: the M1 spike, and the D2 fallback.
- **Maintaining a patch set against AppleWin upstream.** Mitigation: keep patches minimal and apply them on top of the pinned submodule commit (D7).
- **GPL-2.0 obligations** (D6).
- **Hidden sources of non-determinism**, such as the real-time clock, the no-slot clock, random memory initialisation, and AppleWin's speaker feedback (which changes how many cycles each burst runs, depending on the host audio buffer). Mitigation: speaker feedback only runs in windowed realtime mode, stepped mode has no feedback and pins or disables the rest (D9), and M4 tests for it.
- **Agent audio needs AppleWin patches** in `Speaker.cpp` and `MockingboardCardManager.cpp`: tap points, a Mockingboard mix that works with no sound buffer, and capture at the base clock rate. Mitigation: model the patch on the existing WAV output, keep it small, and confirm it in M3 (D12).
- **Realtime input timing depends on when clients send input.** Mitigation: every input reports `applied_at_cycle` (D10). Use stepped mode when a run must be reproducible.
- **Frame timing:** whole-frame capture at vsync may miss mid-frame effects. Accepted for now; per-scanline capture isn't planned.

## Open questions
- Should windowed mode optionally let a person type into the window to take over from the agent? It's view-only for now (D8). Allowing input would need a way to take and hand back control, and runs with human input wouldn't be deterministic.
- Should the agent get the derived text-screen observation (`get_text_screen`) by default? It's a big help to LLM agents, but strictly goes beyond pixels. Currently it's an option on the session (see the API spec).
- Which machine models must be supported? The default is Apple //e Enhanced; others are available through `-model`.
