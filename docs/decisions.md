# Decisions Log

Short records of design decisions. Each one has a status: **Accepted**, **Proposed** (to be confirmed by the milestone named), or **Open**. `project.md` and `api_spec.md` refer to these rather than repeating the reasoning.

---

## D1 — Language and toolchain
**Status:** Accepted

C++20, Visual Studio 2022 (v143 toolset), x64 only. This matches the AppleWin codebase, which the harness links against (see D2).

## D2 — Integration: the harness hosts the AppleWin core
**Status:** Accepted

The harness is its own executable and links AppleWin's emulation core in-process. It replaces the Win32 window by implementing AppleWin's `FrameBase` interface with a headless frontend. The harness owns the main loop.

AppleWin already provides the seams this needs:

| Need | AppleWin hook |
|---|---|
| Frontend abstraction | `source/FrameBase.h`, `GetFrame()` in `source/Interface.h` |
| Frame pixels | `GetVideo().GetFrameBuffer()`, `GetFrameBufferWidth/Height()` (`source/Video.h`) |
| Keyboard | `KeybQueueKeypress()` (`source/Keyboard.h`) |
| Joystick | `JoySetPosition()`, `JoySetButton()` (`source/Joystick.h`) |
| Execution | `CpuExecute()`, driven today by `ContinueExecution()` in `source/Windows/AppleWin.cpp` |
| Save state | `source/SaveState.cpp` |
| Disks / config | `-d1/-d2/-h1/-h2`, `-load-state`, `-noreg`, `-conf`, `-model` (`source/CmdLine.cpp`) |

**Why:** owning the main loop makes `pause`, `step_frames` and deterministic timing trivial: the harness simply stops calling `CpuExecute()`. An IPC plugin inside AppleWin.exe would need extra thread synchronisation to do the same thing.

**Risks the spike must answer:**
- Does the core link cleanly without `Win32Frame`, DirectInput and the registry (`-noreg`)?
- Does it run correctly with no sound buffers at all (headless mode)? AppleWin already handles this case, because it runs when the PC has no audio device.
- How many AppleWin source patches are needed? Keep them as a small, documented patch set.

**Spike outcome (M1):** the spike confirmed D2. The AppleWin core links unchanged into the harness and the AppleWin patch list is empty (no file under `external/AppleWin` is modified). `Win32Frame.cpp` is still compiled into the core library but is never instantiated. Known limitation: AppleWin's debugger assumes the frontend is a `Win32Frame` (`DebugInitialize` calls `Win32Frame::GetWin32Frame` and throws `std::bad_cast` on `HeadlessFrame`), so it is skipped. A later debugger feature needs either an AppleWin patch or a frontend derived from `Win32Frame`.

**Rejected alternatives:**
- *Plugin inside AppleWin.exe:* works, but adds cross-thread pause/step complexity for no benefit over D2.
- *External automation (SendInput + screen capture):* can't pause or step reliably, depends on window state, and can't meet the determinism goal. Dropped entirely, including as a fallback.

## D3 — Transport and authentication
**Status:** Accepted

- JSON-RPC 2.0 over a Windows named pipe: `\\.\pipe\applewin-harness-<session-id>`.
- The pipe ACL allows only the launching user.
- Each client authenticates once, with a `hello` handshake that carries a role and a token. Requests don't repeat the token.
- Optional shared memory carries raw frames.
- TCP/WebSocket are out of scope until there's a concrete remote use case.

**Update (M3 Part A, protocol 0.3):**
- With `--serve`, the harness prints one announcement line on stdout, `{"pipe", "agent_token", "operator_token", "pid"}`, and nothing else. That line is how the tokens reach whoever launched it. They are never written to disk. Remote pipe clients are rejected, and the DACL has one allow ACE for the launching user.
- `hello` carries `protocol_version: "0.3"`.
- Input methods (`type_text`, `press_key`, `set_joystick`) reply at once with `{ input_id, applied_at_cycle }`, where `applied_at_cycle` is `null` unless it is already known. The exact cycles arrive in an `input_applied` notification. A reply deferred until the input was applied would deadlock a single-threaded client in stepped mode, because the input only takes effect during a step. D10 is met through the notification.
- A connection whose outgoing backlog passes 64 MiB is closed, so a stalled client can't freeze emulation.

## D4 — Agent API vs operator API
**Status:** Accepted

The **agent API** is limited to what a human at the machine could do: keyboard, joystick, seeing the screen, and hearing the sound (D12).

The **operator API** covers sessions, disks, checkpoints, stepping and state checksums. The test harness or experiment runner uses it; the agent doesn't.

The role is fixed per connection at `hello`, and the server enforces it.

**MCP adapter (M5):** the MCP adapter (`adapter/applewin_mcp`) is the agent surface. It exposes only agent tools (type, press a key, joystick, wait, look, read the text screen, listen) and no operator method is reachable through it. Operator concerns (the disk, run mode, whether the text screen is allowed) are fixed at launch by command-line flags; the adapter connects as operator itself to create the session and step time, and the agent never sees that connection.

## D5 — Checkpoints replace separate disk snapshots
**Status:** Accepted

Disk II writes are buffered per track in memory, so a mounted image file can be stale. Snapshotting the disk on its own is therefore unreliable.

A **checkpoint** is taken while the emulator is paused, as one operation:
1. Flush disk buffers.
2. Write an AppleWin save state.
3. Copy every working disk image.

Restoring a checkpoint restores all three together.

Original disk images are never written to. Sessions always run on working copies.

### Restore purity (M4)
`restore_checkpoint` must leave the machine and the harness in a state that depends only on the checkpoint. AppleWin's own `Snapshot_LoadState` is not enough, because several things that affect emulation or the captured output are not in the save state. After the load the harness resets each of them:

- **Disks.** Both drives are ejected before the load so AppleWin releases the files. The checkpoint's copies are copied over the working copies, and the save state re-inserts them by path. The load refuses up front (so no file dialog can open) if an image is missing. A failure before the load rolls the disks back and leaves the session usable; a failure in or after it makes the session unusable until `close_session` (D7).
- **Video scanner phase** (`g_dwCyclesThisFrame`). The save state restores it on a restore, but `PowerCycle` did not reset it, so frames after a power cycle differed from a fresh boot's. `Start` and `PowerCycle` now reset it.
- **Text FLASH counter.** A static inside AppleWin's NTSC code, never saved and not reachable. It advances once per scanner wrap (period 32), so the harness counts wraps and moves the phase with a whole-screen redraw to `(emulated_cycles / 17030) mod 32`, the value a stepped run from power-on has.
- **Speaker internals.** The level, the DC filter, the sample buffer and remainder, and the quiet counters are reset to a fresh start's values (`SpkrReset()` plus the harness's own steps). The DC filter is private to AppleWin, so it is driven to its fresh value by pushing samples through it.
- **Mockingboard.** The manager's audio-frame counter is reset, and the mute is cleared.
- **Clock.** AppleWin's clock goes back to the canonical authentic-speed value. The save state's load calls `SetCurrentCLK6502` with whatever speed was set.
- **Enhanced disk speed.** The Disk II card saves it. The session's value is re-applied (D8): speed is a session setting.
- **Random numbers.** `srand(1)` again (D13). The C runtime stream can't be saved, so it is reset deterministically.
- **Harness state.** The input queue is emptied (each discarded input reports `discarded: true`), held keys and buttons are released, the joystick returns to its position at the checkpoint, the audio capture and the sound detectors are cleared, and the presented frame is re-rendered from the restored video memory.
- **Cycle origin.** The power-on origin is stored in the checkpoint, so `emulated_cycles` continues from the checkpoint's value.

Consequence (D2 of the M4 rulings): a replay is exact only between two restores of the same checkpoint, because capture placement and the random stream restart at the restore. A straight run is not comparable with a restored one for weak-bit disk reads.

**D10 (M4): non-ASCII working paths.** AppleWin saves image paths as UTF-8 but loads them with the ANSI file API, so a checkpoint whose working copy has a non-ASCII path could never be restored. `checkpoint` refuses (`-32004`) when a working path's UTF-8 bytes differ from its ANSI bytes. Cost: a user whose `%TEMP%` path has non-ASCII characters cannot checkpoint. Parked mitigation (M6): insert working copies through their ASCII 8.3 short paths when those exist.

**D11 (M4): startup sweep.** A killed server cannot delete its work directory. On start, the harness removes `%TEMP%\applewin-harness\<pid>` directories and `<pid>.ini` files whose process is no longer running. Entries of live processes are skipped, so a reused pid can leave a stale directory behind but never causes a live one to be deleted.

## D6 — Licensing
**Status:** Open — must be decided before M1 code is published

AppleWin is GPL-2.0. Under D2 the harness links AppleWin code, so the harness must be distributed under a GPL-2.0-compatible license.

## D7 — How the harness finds AppleWin source
**Status:** Accepted

AppleWin is a git submodule at `external/AppleWin`, pointing at `https://github.com/AppleWin/AppleWin.git` and pinned to a specific commit. Every build and CI run uses exactly the same AppleWin source, with no environment variable or separate checkout needed.

- **Current pin:** `6c579da5` (v1.32.1.0-2, upstream master as of 2026-10-16).
- **Upgrading:** moving the pin is a deliberate change. Run `git -C external/AppleWin checkout <commit>`, then `git add external/AppleWin`. The M2 golden tests must pass at the new commit.
- **Patches:** any AppleWin patches the harness needs (D2) live in this repository and are applied on top of the pinned commit. Never commit inside the submodule. If a fork becomes unavoidable, point the submodule URL at the fork and record that here.

## D8 — Display modes: windowed with sound (default) and headless
**Status:** Accepted

The harness runs in one of two display modes, chosen when it starts:

- **Windowed** (default): opens a window showing the live Apple screen and plays the sound the Apple makes (speaker, Mockingboard and other sound cards), so a person can watch and listen to what the agent is doing.
- **Headless** (`--headless`): no window and no sound. Use it for CI, batch runs, and machines without a desktop or audio device.

**How it works:**
- Both modes use the same `FrameBase` implementation, `HarnessFrame` (renamed from the M1 headless frame), and the same harness-owned main loop (D2). The only differences are a small *presenter* and the sound output.
- In windowed mode the presenter is the view-only `Window`: it draws the latest presented frame with `StretchDIBits` and pumps window messages on the harness thread. Headless mode simply has no presenter.
- Sound uses AppleWin's existing seam, `FrameBase::CreateSoundBuffer`, in every mode. AppleWin is given a virtual buffer, never a real DirectSound one (D14). In windowed mode the harness's own `AudioOutput` plays the mixed result and counts underruns of its own buffer; headless has no `AudioOutput` and opens no device.
- **How underruns are measured:** `AudioOutput` counts an underrun when the play cursor of its own buffer passes the end of the written data after playback has started. Startup and resync after a pause aren't underruns.
- AppleWin's own `Win32Frame` (with its menus, dialogs and message loop) is deliberately not reused. It would take control of the main loop away from the harness.

**Rules:**
- The window is **view-only**. Keyboard and mouse input to the window doesn't reach the emulator, so the agent API stays the only source of input and runs stay deterministic.
- The display mode does not affect emulated state. In stepped mode, a headless run and a windowed run produce identical frames, state checksums and audio. (The SSI263 speech exception of earlier drafts is gone: with virtual buffers (D14) the phoneme IRQ no longer depends on host timing.) In realtime mode the two can differ in where input lands, because of the audio feedback (D9).
- Sound plays through the speakers only in realtime mode. In stepped mode emulation doesn't run in real time, so playback is muted.
- The agent's audio (D12) is captured separately, in every mode, and doesn't depend on whether sound is playing.
- If windowed mode can't open an audio device, the harness logs a warning and carries on without sound.
- In stepped mode the window keeps showing the last frame while the emulator is paused.
- Closing the window shuts the harness down cleanly, the same as `close_session` followed by exit. Original disk images are never touched (D5).

## D9 — Execution pacing: realtime and stepped modes
**Status:** Accepted

AppleWin times emulation by counting 6502 cycles. The video scanner, Disk II, speaker and 6522 timers all run off emulated cycles, and the wall clock only sets the pace (`ContinueExecution()` in `source/Windows/AppleWin.cpp`). The harness keeps that split.

**Realtime mode** copies AppleWin's loop:
- Each burst runs `g_fCurrentCLK6502 × 1 ms` cycles through `CpuExecute`, then waits for the next 1 ms tick.
- The tick comes from a high-resolution Windows waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`). AppleWin uses a DirectShow `IReferenceClock` in `SoundCore.cpp` instead. The waitable timer works the same way in both display modes, including on machines with no audio device.
- Speaker feedback now comes from the harness's `AudioOutput` (D14) and is added to each burst's cycle count. AppleWin's own `g_nCpuCyclesFeedback` stays 0 and is unused, because its buffers always report the ideal fill.
  - **Windowed mode:** after each burst (`AudioOutput::Pump` runs after every realtime step), the feedback adjusts the next burst's size according to how full the harness's DirectSound buffer is. It uses AppleWin's rule, with cycles per sample truncated as AppleWin does. That keeps sound free of gaps and crackle, and makes the sound card the long-term clock reference.
  - **Headless mode:** there's no `AudioOutput`, so the feedback stays at zero and every burst is the same size.
  - **The trade-off:** in windowed realtime mode, burst sizes depend on the host's audio device. That only changes *where* input lands, which realtime mode can't make reproducible anyway, and `applied_at_cycle` records it (D10).
- AppleWin's full-speed conditions still apply (see D11).

**Stepped mode** has no timer:
- `step_frames(n)` runs exactly `n × NTSC_GetCyclesPerFrame()` cycles. That's 17,030 cycles per frame at 60 Hz (65 × 262).
- Video is always rendered (`bVideoUpdate = true`), even when a full-speed condition would apply.
- Stepped runs always use authentic speed and enhanced disk access (AppleWin's defaults); speed settings (D11) affect realtime runs only, so they can't change stepped results: frames, CPU state or captured audio (ruling C7). Both matter otherwise: `g_fCurrentCLK6502` sets the speaker's and Mockingboard's samples per cycle, and enhanced disk access changes AppleWin's disk timing.
- Sound playback is muted (D8), and the speaker feedback isn't used. Agent audio capture (D12) continues.

## D10 — Emulated time and input timing
**Status:** Accepted

The API measures time in emulated time:
- `emulated_cycles` counts cycles since power-on, using AppleWin's `g_nCumulativeCycles`.
- `emulated_ms` is `emulated_cycles` divided by the **base** clock: `CLK_6502_NTSC` (≈1.0205 MHz) or `CLK_6502_PAL`. It does not depend on the speed setting.

All durations in the API (`hold_ms`, `inter_key_delay_ms`, `duration_ms`) are emulated milliseconds, converted to cycles. As a result:
- A script gives identical results at any speed setting and in either run mode.
- At 2× speed in realtime mode, a 50 ms key hold lasts 25 ms of wall-clock time.

Input is applied between bursts or steps. Every input response reports `applied_at_cycle`.
- **Stepped mode:** this is the paused cycle count, so it's fully deterministic.
- **Realtime mode:** when input lands depends on when the client sends it. The cycle stamp records exactly when it was applied, so a run can be analysed afterwards and, later, replayed.

## D11 — Speed controls mirror AppleWin exactly
**Status:** Accepted

The operator API offers exactly the speed controls AppleWin has, with AppleWin's own value ranges. It implements them by calling AppleWin's own functions, so behaviour (including clamping and rounding) matches exactly.

| AppleWin control | AppleWin source | Harness API |
|---|---|---|
| Config ▸ "Authentic machine speed" / "Custom speed" slider (0–40) | `g_dwSpeed`, `SetCurrentCLK6502()` (`Core.cpp`) | `set_speed { machine_speed: 0..40 \| "authentic" \| "max" }` |
| `-clock-multiplier <x>` command-line option | `UseClockMultiplier()` (`Core.cpp`) | `set_speed { clock_multiplier }` |
| Config ▸ "Enhance disk access speed" (default on) | `Disk2CardManager::SetEnhanceDisk()` | `set_speed { enhance_disk_speed }` |
| Scroll Lock: hold for full speed, or toggle it with the Scroll Lock toggle option | `g_bScrollLock_FullSpeed` / `GetKeyState(VK_SCROLL)` | `set_speed { full_speed_override }` |
| Config ▸ "50Hz video" / `-50hz` / `-60hz` (changes the base clock between PAL and NTSC; AppleWin restarts the emulator) | `Get6502BaseClock()` | `create_session { speed: { video_refresh: 50 \| 60 } }` — set once per session |

**Harness rulings (M3 Part C):**
- `get_speed.clock_multiplier` is `effective_clock_hz / base_clock_hz`, and null at `machine_speed` 40.
- `full_speed_active` is true only while an open realtime run is stepping at full speed. The condition alone (`full_speed_reasons`) can be non-empty without it.
- `frame_ready` is capped at one per 16 ms of wall time per subscription while stepping at full speed, matching AppleWin's full-speed redraw interval.
- An empty Disk II drive keeps its motor on, so with `enhance_disk_speed` (default true) a realtime or windowed run with no disk spins at full speed. This is AppleWin's rule, not a harness addition.
- Windowed playback happens only at authentic speed (`machine_speed` 10) and not at full speed (see D14).
- Ruling C7: stepped runs always use authentic speed and enhanced disk access; speed settings affect realtime runs only. The session's machine speed is held by the harness and applied to AppleWin (`g_dwSpeed`, then `SetCurrentCLK6502()`, as AppleWin's config dialog does) at the start of every realtime step; every stepped step applies speed 10. AppleWin's clock therefore changes only when a step needs another one, because `SetCurrentCLK6502` also re-initializes the speaker (it drops a partial sample) and the Mockingboard clock. `clock_multiplier` uses a copy of `UseClockMultiplier`'s conversion for the same reason, clamped at 4.0 first (AppleWin's `(ULONG)(x × 10)` is undefined for huge values; 3.9 and above give 39 anyway); a test pins it to AppleWin's function. A stepped run turns enhanced disk access on and restores the session's setting when it ends. Cost: a stepped run can't reproduce enhance-off disk timing.

**How the slider value maps to speed** (`SetCurrentCLK6502`):
- Values 0–9 give a multiplier of `0.5 + 0.05 × value`.
- Values 10–39 give `value / 10`. Value 10 is authentic speed.
- 40 is `SPEED_MAX`: the clock rate is ignored and emulation runs unthrottled.
- The resulting clock is `base clock × multiplier`.

**How `clock_multiplier` maps to the slider** (`UseClockMultiplier`):
- Below 0.5, it's raised to 0.5.
- Below 1, the slider becomes `(int)((x − 0.5) × 20)`.
- Otherwise the slider becomes `(int)(x × 10)`, capped at 39.
- So this option can't select unthrottled speed, the same as in AppleWin.

**When emulation runs at full speed** (`g_bFullSpeed`, realtime mode only):
- `machine_speed` is 40, **or**
- `full_speed_override` is on, **or**
- a Disk II motor is on and `enhance_disk_speed` is on, provided the speaker and Mockingboard are silent.

While at full speed the timer is skipped and frames are redrawn the way AppleWin does it (`VideoRedrawScreenDuringFullSpeed`). `frame_ready` notifications are limited to 60 per wall-clock second. As in AppleWin, Mockingboard output is muted while at full speed.

**What isn't mirrored:**
- AppleWin's choice between holding and toggling Scroll Lock. That's a keyboard UI detail, and the `full_speed_override` setting covers both.
- The debugger's full-speed stepping, because there's no debugger API.

**Ownership:**
- Speed settings belong to the operator API (D4). Scroll Lock and the Config dialog are emulator controls, not Apple II I/O.
- They are session settings. `restore_checkpoint` doesn't change them.

## D12 — The agent can hear
**Status:** Accepted. Updated in M3 Part B: capture goes through virtual sound buffers (D14), not AppleWin patches.

Sound can be an important cue for the agent: a bell on an error, a beep on a prompt, music changing in a game. So the agent API lets the agent hear the Apple. Audio counts as human I/O (D4), so no session option is needed to turn it on.

**Capture at the source, in emulated time:**
- AppleWin's speaker, Mockingboard and SSI263 code all write their samples into sound buffers they get from `FrameBase::CreateSoundBuffer`. The harness hands out virtual buffers (D14) and `AudioCapture` records every commit to them, before anything reaches a real device. **No AppleWin patch is needed.**
- Commits are placed on the timeline by the cycle interval between commits, using each voice's own sample rate (D14). Because AppleWin always sees the ideal buffer fill, the samples AppleWin produces depend only on emulated time. Where they land on the timeline also depends on the commit boundaries (see below).
- Capture runs in every display mode and run mode (windowed, headless, realtime, stepped).

**Format of the captured audio:**
- 16-bit PCM, stereo, 44,100 samples per emulated second, measured against the base clock (D10). The speaker is mono and is mixed equally into both channels; Mockingboard is stereo.
- The capture timeline is in emulated time whatever the speed setting: each commit is placed by the emulated cycles it covers, so a program sounds the same at any speed. Part C's speed-change test (`realtime` suite) checks this.
- The harness keeps the most recent 30 emulated seconds in a ring buffer.

**What this guarantees:** captured audio is byte-identical for the same emulated state and the same sequence of run commands (the same step sizes). In stepped mode the same script with the same step partition produces byte-identical audio in windowed and headless mode, at any speed setting. M4 checks this.

**What it does not guarantee (ruling B13):** splitting the same run differently (for example `step_frames 120` versus 60 + 60) can change individual samples by +/-1 LSB, for about 170 ms after the split. Each commit is resampled over the interval since the previous commit (B2), and commit boundaries are step boundaries. The difference is inaudible. Anchoring each voice by its own cumulative sample count would remove the dependence; that is the planned improvement if M4 checkpoint replay needs split-independence. Realtime capture likewise depends on burst sizes.

**Sound events:** an LLM agent can't usefully poll raw PCM, so the harness also detects when sound starts and stops. It does this from the captured stream, in emulated time, so the events are deterministic too.
- A `sound_started` event fires when the level rises above a threshold.
- A `sound_stopped` event fires once it has stayed below the threshold for a minimum time. It carries the duration, the peak level and an estimate of the dominant frequency, which is enough to tell a bell from a click or a tune.

The API details are in `api_spec.md` (Sound section).

## D13 — Pinned power-on randomness
**Status:** Accepted

At every power-on, AppleWin seeds the zero-page random bytes RNDL/RNDH (`$4E`/`$4F`) from `rand() ^ timeGetTime()` (`Memory.cpp`, ORed with `0x20` as a workaround for "Pooyan"). That made frame hashes alternate between runs of the same script.

After `Start` and after each `PowerCycle`, the harness sets both bytes to `0x20` (AppleWin's floor value, so the Pooyan workaround still holds) and calls `srand(1)`.

**Consequence:** runs are reproducible. Programs that read RNDL/RNDH at power-on see a fixed value rather than a random one. No AppleWin patch is needed.

## D14 — Virtual sound buffers
**Status:** Accepted

AppleWin never gets a real DirectSound buffer. The harness replaces the translation unit `external/AppleWin/source/Windows/DXSoundBuffer.cpp` in `AppleWinCore` with its own `src/host/virtual_sound.cpp`, the same way M1 replaced `AppleWin.cpp`. No AppleWin file is modified. `virtual_sound.cpp` defines every symbol the rest of the core uses from the original (`DSInit` succeeds and opens no device, `GetDefaultAudioDeviceName` returns `"harness"`, and so on).

**Virtual buffers:** `HarnessFrame::CreateSoundBuffer` returns a `VirtualSoundBuffer` in every mode: a memory ring of the requested size, rate and channels. Its cursors are derived so AppleWin always sees the ideal fill: the play cursor is the end of the last commit minus 3/8 of the buffer, and the write cursor is the end of the last commit (ruling B6, so a voice that starts at the write cursor, like the Mockingboard, sees the ideal fill from its first call). Nothing is dropped, AppleWin's error correction stays at 0 and `g_nCpuCyclesFeedback` is 0.

**Capture:** each commit (except a whole-buffer zero fill, which is AppleWin re-initialising the buffer) is appended to `AudioCapture`.
- Voices (speaker, Mockingboard, SSI263) register with their own sample rate (`RegisterVoice`). They are mixed additively with saturation into one 44,100 Hz stereo int16 timeline. Each voice's samples are clamped to int16 first, and other rates are resampled linearly.
- A commit is placed by the cycle interval since the voice's previous commit (B2), not by a fixed rate: the speaker really runs at about clock/23, near 44,369 Hz.
- Gap rule (B7): if the interval exceeds the commit's own length plus 100 ms of cycles, the voice was paused. The gap stays silent and the commit is placed as a first commit.
- The timeline is held in a 30 s ring.

**Playback (windowed, realtime runs only):**
- `AudioOutput` streams the mixed timeline to a harness-owned DirectSound buffer (`dsound.dll` is delay-loaded; headless never loads it). It has `GLOBALFOCUS`, so sound continues when the view-only window is in the background.
- It plays 110 ms behind the emulated clock (B11), so slow Mockingboard commits are heard. In practice the audio lags the window by about 200 to 300 ms.
- Burst-size feedback keeps that buffer between 1/4 and 1/2 full. `AudioOutput::FeedbackCycles` computes it from the fill before each write, with AppleWin's rule and constants (`SoundCore_GetErrorInc`/`GetErrorMax`) and cycles per sample truncated as AppleWin truncates them. It is read once per realtime burst, and only when `AudioOutput` exists.
- Underruns are counted only after playback has started, when the play cursor passes the end of the written data. `audio underruns` reports this count. `MonitoredSoundBuffer` is deleted.
- Outside realtime runs the buffer is stopped, so it is silent (B12). Resuming resynchronises, and that isn't an underrun.
- Speed (D11, ruling C6): playback runs only at `machine_speed` 10 and not at full speed. The capture timeline is in emulated time, so at other rates it cannot play in step with the wall clock without resampling, and the burst feedback would pull emulation back to authentic speed. At other speeds the buffer is stopped. Agent capture is unaffected. Resampled playback is parked for later.

**Sound events:** the level detector runs up to the smaller of the emulated clock and 110 ms behind the newest committed audio (B9), matching the gap rule, because a voice can commit up to about 64 ms after another. Level is the AC RMS of each 10 ms block after removing the block's mean and linear trend (B8). AppleWin's speaker ramps its DC level back to zero after a sound, and that ramp must not count as sound.

**Consequences:**
- D8: the SSI263 exception is gone; stepped runs are identical in both display modes, including audio.
- D9: the realtime feedback comes from `AudioOutput`.
- D12: no AppleWin patches; capture goes through the virtual buffers.
- Maintenance: the replacement TU must track AppleWin's `DXSoundBuffer` interface whenever the submodule pin moves (D7).
