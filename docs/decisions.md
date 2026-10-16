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

## D5 — Checkpoints replace separate disk snapshots
**Status:** Accepted

Disk II writes are buffered per track in memory, so a mounted image file can be stale. Snapshotting the disk on its own is therefore unreliable.

A **checkpoint** is taken while the emulator is paused, as one operation:
1. Flush disk buffers.
2. Write an AppleWin save state.
3. Copy every working disk image.

Restoring a checkpoint restores all three together.

Original disk images are never written to. Sessions always run on working copies.

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
- Sound uses AppleWin's existing seam. AppleWin asks the frontend for each sound buffer through `FrameBase::CreateSoundBuffer`. In windowed mode the harness returns AppleWin's own DirectSound buffer (`Windows/DXSoundBuffer.cpp`), just as `Win32Frame` does, wrapped in a `MonitoredSoundBuffer`. The wrapper mutes the buffer outside realtime runs (D8 rules below) and counts underruns. In headless mode it returns `nullptr`, which takes AppleWin's existing no-sound path (`DSGetSoundBuffer` in `SoundCore.cpp`).
- **How underruns are measured:** the wrapper counts an underrun per buffer, only after playback has visibly started (the play cursor has moved). It mirrors AppleWin's unsafe-zone check (`SoundCore_ValidateAndAlignWriteOffset`): the end of the last committed write must not lie between the play and write cursors. The startup realignment of a freshly created buffer isn't an underrun.
- AppleWin's own `Win32Frame` (with its menus, dialogs and message loop) is deliberately not reused. It would take control of the main loop away from the harness.

**Rules:**
- The window is **view-only**. Keyboard and mouse input to the window doesn't reach the emulator, so the agent API stays the only source of input and runs stay deterministic.
- The display mode does not affect emulated state, with one exception: SSI263 speech chips on Mockingboard cards. Headless, `SSI263::Update` returns early without DirectSound, so the phoneme-complete IRQ never fires. Windowed, that IRQ is driven partly by the DirectSound play position, which depends on host timing. So for programs that use speech, headless and windowed runs differ, and stepped windowed runs are not reproducible. Otherwise, in stepped mode, a headless run and a windowed run produce identical frames and state checksums. In realtime mode the two can differ in where input lands, because of the audio feedback (D9).
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
- AppleWin's speaker feedback (`g_nCpuCyclesFeedback`) is kept, as in AppleWin.
  - **Windowed mode:** the feedback adjusts each burst's size according to how full the DirectSound buffer is. That keeps sound free of gaps and crackle, and makes the sound card the long-term clock reference.
  - **Headless mode:** there's no sound buffer, so the feedback stays at zero and every burst is the same size.
  - **The trade-off:** in windowed realtime mode, burst sizes depend on the host's audio device. That only changes *where* input lands, which realtime mode can't make reproducible anyway, and `applied_at_cycle` records it (D10).
- AppleWin's full-speed conditions still apply (see D11).

**Stepped mode** has no timer:
- `step_frames(n)` runs exactly `n × NTSC_GetCyclesPerFrame()` cycles. That's 17,030 cycles per frame at 60 Hz (65 × 262).
- Video is always rendered (`bVideoUpdate = true`), even when a full-speed condition would apply.
- Speed settings (D11) therefore can't change emulated results in stepped mode.
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
**Status:** Accepted. The tap points are confirmed in M3.

Sound can be an important cue for the agent: a bell on an error, a beep on a prompt, music changing in a game. So the agent API lets the agent hear the Apple. Audio counts as human I/O (D4), so no session option is needed to turn it on.

**Capture at the source, in emulated time:**
- AppleWin builds speaker samples from emulated cycles (`Speaker.cpp`: samples queued into `g_pSpeakerBuffer`) and mixes Mockingboard output into `MockingboardCardManager::m_mixBuffer`. Both happen *before* anything is sent to DirectSound.
- The harness captures audio at those two points, through a small AppleWin patch (D2, D7). AppleWin's existing `-wav-speaker` / `-wav-mockingboard` WAV output (`Riff.cpp`) is the model for this patch. The patch must:
  - Pass each block of samples to the harness, tagged with its emulated cycle.
  - Still produce the Mockingboard mix when there is no sound buffer. Today `MockingboardCardManager` returns early if `DSGetLock` fails, which would leave headless mode silent.
  - Capture before AppleWin's output adjustments: the audio-buffer feedback that adds or drops samples, the full-speed Mockingboard mute, and volume.
- Capture runs in every display mode and run mode (windowed, headless, realtime, stepped).

**Format of the captured audio:**
- 16-bit PCM, stereo, 44,100 samples per emulated second, measured against the base clock (D10). The speaker is mono and is mixed equally into both channels; Mockingboard is stereo.
- AppleWin computes speaker samples at the *current* clock (`g_fClksPerSpkrSample`). At speeds other than authentic, the harness patch computes the capture stream at the base clock instead, so the same program sounds the same to the agent at any speed.
- The harness keeps the most recent 30 emulated seconds in a ring buffer.

**What this guarantees:** agent audio depends only on emulated state. In stepped mode the same input script produces byte-identical audio in windowed and headless mode, at any speed setting. M4 checks this.

**Sound events:** an LLM agent can't usefully poll raw PCM, so the harness also detects when sound starts and stops. It does this from the captured stream, in emulated time, so the events are deterministic too.
- A `sound_started` event fires when the level rises above a threshold.
- A `sound_stopped` event fires once it has stayed below the threshold for a minimum time. It carries the duration, the peak level and an estimate of the dominant frequency, which is enough to tell a bell from a click or a tune.

The API details are in `api_spec.md` (Sound section).

## D13 — Pinned power-on randomness
**Status:** Accepted

At every power-on, AppleWin seeds the zero-page random bytes RNDL/RNDH (`$4E`/`$4F`) from `rand() ^ timeGetTime()` (`Memory.cpp`, ORed with `0x20` as a workaround for "Pooyan"). That made frame hashes alternate between runs of the same script.

After `Start` and after each `PowerCycle`, the harness sets both bytes to `0x20` (AppleWin's floor value, so the Pooyan workaround still holds) and calls `srand(1)`.

**Consequence:** runs are reproducible. Programs that read RNDL/RNDH at power-on see a fixed value rather than a random one. No AppleWin patch is needed.
