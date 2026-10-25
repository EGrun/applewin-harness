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
- Input methods (`type_text`, `press_key`, `press_keys`, `set_joystick`) reply at once with `{ input_id, applied_at_cycle }`, where `applied_at_cycle` is `null` unless it is already known. The exact cycles arrive in an `input_applied` notification. A reply deferred until the input was applied would deadlock a single-threaded client in stepped mode, because the input only takes effect during a step. D10 is met through the notification.
- A connection whose outgoing backlog passes 64 MiB is closed, so a stalled client can't freeze emulation.

## D4 — Agent API vs operator API
**Status:** Accepted

The **agent API** is limited to what a human at the machine could do: keyboard, joystick, seeing the screen, and hearing the sound (D12).

The **operator API** covers sessions, disks, checkpoints, stepping and state checksums. The test harness or experiment runner uses it; the agent doesn't.

The role is fixed per connection at `hello`, and the server enforces it.

**MCP adapter (M5):** the MCP adapter (`adapter/applewin_mcp`) is the agent surface. It exposes only agent tools (type, press a key, joystick, wait, look, read the text screen, listen) and no operator method is reachable through it, apart from the floppy swap below. Operator concerns (the disk loaded at launch, run mode, whether the text screen is allowed) are fixed at launch by command-line flags; the adapter connects as operator itself to create the session and step time, and the agent never sees that connection.

**Disk swapping (M5 addendum):** putting a floppy in a drive or taking it out is something a person at the machine does, so `insert_disk` and `eject_disk` are agent tools in the adapter (the adapter sends them over its operator connection, because they are operator methods in the harness). The disks loaded at launch and checkpoints stay operator-only. `export_disk` was added as an agent tool later, at the user's request, so an agent can save its disk results; it is the one agent tool that writes outside the machine, so the harness's own path checks (absolute path, no overwrite, not an original image, not its work directory, no device or stream names) stay in force and the adapter adds the UNC/device rejection. A `set_speed` tool (realtime sessions only; `machine_speed` and `enhance_disk_speed`) was added the same way, so an agent can speed up or slow down a running program; stepped results don't depend on speed (D9), so it can't affect a stepped run.

**Checkpoints for the agent, stepped sessions only (2026-10-20):** at the user's request the adapter exposes `checkpoint`, `restore_checkpoint`, `list_checkpoints` and `delete_checkpoint` as agent tools, sent over its operator connection like the floppy tools. They exist only in a stepped session: with `--realtime` they are not registered at all, because restoring in a realtime game would be a power a player at the machine does not have (the user first ruled checkpoints out as cheats on 2026-10-19, then allowed them for stepped mode, which is a practice and analysis mode where the machine already stands still between calls). They reuse the harness checkpoints (D5: AppleWin's save state plus the working disks, restored deterministically), so a manoeuvre retried from the same checkpoint with the same input replays exactly. Memory reads stay out of reach in both modes.

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

**D10 (M4): non-ASCII working paths.** AppleWin saves image paths as UTF-8 but loads them with the ANSI file API, so a checkpoint whose working copy has a non-ASCII path could never be restored. `checkpoint` refuses (`-32004`) when a working path's UTF-8 bytes differ from its ANSI bytes. Cost: a user whose `%TEMP%` path has non-ASCII characters cannot checkpoint. Mitigation (M6): when a working copy's long path is not ANSI-safe, `Emulator::InsertDisk` hands AppleWin its ASCII 8.3 short path (`GetShortPathNameW`), so the save state names a path the load can open; `list_disks.working_path` still reports the long path. `checkpoint` refuses only when no ASCII path exists (8.3 names disabled on the volume). A path with no ANSI form and no short name now fails the insert with a clear message instead of the raw conversion error.

**D11 (M4): startup sweep.** A killed server cannot delete its work directory. On start, the harness removes `%TEMP%\applewin-harness\<pid>` directories and `<pid>.ini` files whose process is no longer running. Entries of live processes are skipped, so a reused pid can leave a stale directory behind but never causes a live one to be deleted.

## D6 — Licensing
**Status:** Accepted (2026-10-18)

AppleWin is GPL-2.0-or-later. Under D2 the harness links AppleWin code, so the compiled binaries must be distributed under GPL-2.0-or-later. The user decided: "use a split MIT license where possible, and GPL2 compatible otherwise. Do not include disk images in the repo."

- **MIT for what the project wrote.** `src/`, `tests/`, `adapter/`, `tools/` and `docs/` are MIT (`LICENSE`), and each owned source file carries `SPDX-License-Identifier: MIT`. MIT is GPL-2.0-compatible, so the combined work is allowed, and it keeps the project's own code reusable on permissive terms (for example the Python adapter, which does not link AppleWin).
- **GPL where required.** `AppleWinHarness.exe` and `HarnessTests.exe` link AppleWin, so as combined works they are GPL-2.0-or-later (`LICENSE-GPL-2.0.txt`, copied from the submodule). `NOTICE.txt` carries the source offer: this repository's URL and release commit, plus the pinned AppleWin commit (D7).
- **Third-party notices.** `THIRD-PARTY-NOTICES.txt` lists everything linked into the exe (AppleWin, zlib, minizip, libyaml, nlohmann/json) with license texts, and names the adapter's runtime dependency `mcp` (MIT, installed by pip) and doctest (test binary only).
- **ROM images.** AppleWin compiles Apple II ROM images into the exe as resources. The bundle includes them on the same terms as AppleWin's own releases (risk recorded in `project.md`).
- **No disk images** in the repository or the release bundle. Tests use the images in the AppleWin submodule. The DOS 3.3 System Master is in the AppleWin repository's `bin/` directory.

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

**Keys and the strobe (post-M6 fix, 2026-10-19):** M3 made every key wait until the keyboard strobe was clear. A program that stops reading the keyboard (a game mid-animation) then left one unread `press_key` at the head of the queue for ever, and everything behind it (keys, joystick changes, RESET) with it. The rules are now:
- **`press_key` does not wait for the strobe.** It overwrites the keyboard latch, as a keypress does on a real //e (AppleWin's `KeybQueueKeypress`): a key the program has not read yet is lost.
- **`type_text` still waits for the strobe**, key by key, so typed text arrives whole and in order (like AppleWin's paste).
- **A `type_text` key that is due but not taken within 600 frames is dropped with the rest of its text**, reported once as `input_applied { …, discarded: true }` (the D1 shape). Nothing else in the queue waits on the program, so the queue always drains on its own.
- RESET stays in queue order: with the two rules above it can wait at most for one text timeout.

**Key sequences (post-M6, 2026-10-19):** in a realtime session an agent's think time between calls is seconds, so a manoeuvre sent as separate `press_key` calls (Conan: ESC, LEFT, LEFT, then a running jump Q at a precise spot, then A for the ladder) landed with about 0.25 s of jitter. `press_keys` queues the whole sequence as one input, timed in emulated time:
- **It is the same queue entry as `type_text`.** A text is a key sequence built from its characters (gap `inter_key_delay_ms`, no hold); `press_keys` sets a gap and a hold per key. Both wait for the program to take each key before the next, so a fast sequence never overwrites a key the program has not seen (LEFT, LEFT stays two presses), and both drop the rest after 600 frames unread. (How `press_keys` tells a key was taken changed the same day: see the next note.)
- **Applied and read are different moments.** A key is *applied* when the harness puts it in the keyboard latch; the program *reads* it when it clears the strobe, which can be frames later if it polls slowly, and that is when it acts. `press_keys` reports both cycles per key, so an agent can measure the gap the program actually saw. "Read" is observed at step boundaries (within one step, about 1 ms) without patching AppleWin.
- **A sequence is done when its last key is read** (and every hold released), so the read cycle of the last key is known; a last key unread for 600 frames discards the sequence. `type_text` still completes when its last key is applied (unchanged M3 timing) and reports no per-key cycles.
- RESET is not allowed in a sequence; it stays a single `press_key`.

**Read is a keyboard read, not a strobe clear (post-M6 fix, 2026-10-19):** the first `press_keys` waited for the strobe, and counted a key as read when the strobe cleared. Conan reads `$C000` and acts on a climb key but never clears the strobe during or after the climb, so a sequence ending in that key timed out (dropping later keys and leaving the game running unpaused for 10 s), and the next sequence's first key stalled on the stale strobe. Now:
- **A `press_keys` key is read at the program's first `$C000`–`$C00F` read after it was pressed.** A program must read `$C000` to see a key at all, so that is when it sees it. The harness wraps AppleWin's `IORead[0]` (the `$C00x` read handler, public in `Memory.h`) with a pass-through that only notes the read; it changes neither the value nor the timing, so stepped runs stay deterministic, and it is reinstalled at each step boundary because AppleWin rebuilds the table at start and on a snapshot load. The read is still seen at the next step boundary (within about 1 ms), so a program that reads and then clears the strobe within the same step has done both before the next key goes in.
- **The first key waits for nothing**, like `press_key`: a sequence only protects its own keys, and a stale strobe left by another input can no longer stall it.
- **The timeout is 600 frames after the unread key was pressed.** A timed-out sequence's discarded report lists the keys that were pressed, with the unread one's `read_cycle` null, so an agent can tell an ignored key from one that was acted on.
- `type_text` is unchanged: it still waits for the strobe to clear before each key, which is what keeps typed text whole in a program that reads lines (GETLN clears the strobe).

**Joystick entries and wait_for_read (2026-10-19):** a game manoeuvre often mixes keys with a joystick move or button, and sometimes needs a key pressed over one the game has not yet taken. `press_keys` entries may now be joystick changes or keys with `wait_for_read: false`:
- **One list, one clock.** A joystick entry is queued in the same sequence, so `delay_ms` counts from the previous entry's applied cycle whatever its kind. It waits for no read. With `duration_ms` it queues a `JoyRestore` of the state just before it (as `set_joystick` does), marked `completes = false` and carrying the sequence id; the sequence completes only when its last such restore (like its last key release) is done, and a discard of the queue does not report it twice.
- **"The previous key" is the most recent key entry.** A key with `wait_for_read` waits for the read of the most recent key before it; joystick entries in between do not reset that. A key overwritten by a later `wait_for_read: false` key keeps `read_cycle: null` and never times out; the 600-frame timeout applies only to a key a waiting entry depends on, or to the last key of the sequence.
- **Wire shape.** `input_applied.keys` has one item per entry in request order; a joystick item is `{ applied_cycle, joystick: true }` with no `read_cycle`, so a never-read key (`read_cycle: null`) is not confused with it.

**Read-anchored delays and exact timing (2026-10-20):** a tester's Conan script, tuned in a stepped session, drifted in realtime by up to 115 ms although every key went in on time. A program acts on a key when it reads it, and its poll loop reads the key up to a whole loop period after it went in (the title screen polls about every 90 ms). How long depends on where in the loop the key arrived: a stepped run queues a sequence at the same cycle every time, so a timing tuned there silently includes that wait; in realtime the sequence starts at a wall-clock-dependent cycle. Measured with a 90.5 ms poll loop, two keys 200 ms apart (counted from the apply) were 181 or 271 ms apart as the program saw them, depending only on that phase; counted from the program's read of the first key they were 271 ms apart at every phase. So:
- **`delay_from: "read"`**, for a whole sequence or one entry: the delay counts from the program's read of the most recent key before the entry. Sequence-level, because the drift is not limited to keys that restart a game's clock (start, unpause): any two keys of a slowly polling program drift by a whole poll period. The default stays `"applied"`, so scripts already tuned keep their meaning.
- **Exact read cycles.** The keyboard tap records the cycle of the read itself (`CpuCalcCycles(nExecutedCycles)`, as AppleWin's own paddle handler does to time its reads), not the step boundary after it. Anchoring to a boundary-rounded read would keep the step grid in the timeline, and that grid differs between the modes (1,020-cycle steps stepped; about 1 ms plus the audio feedback in realtime).
- **Exact application.** A step ends at the next press_keys due cycle (an entry's due cycle, a hold's release, a restore, the completion), so an entry goes in on its cycle, to within one instruction, in both modes. `press_key`, `set_joystick` and `type_text` keep taking effect at the start of the next step (D10). An entry that becomes due only when a read is seen cannot go in before the end of the step in which the read happened (the CPU is not stopped mid-step).
- **A 1 ms guard after a read.** With exact application a key waiting for a read could go in a few cycles after it, and the first test run lost a key that way: BASIC checks `$C000` (the read), then fetches the key and clears the strobe a few instructions later, and the next key replaced it in between. Before, keys went in at step starts, so the program nearly always had most of a step to finish. Now a key that waits for a read goes in no sooner than 1 ms (1,020 cycles) after it, in both modes, which keeps the old guarantee that `wait_for_read` never loses a key and is the same in every run. (With the default `"applied"` such a key can therefore go in up to 1 ms later than it used to, when it went in at the next step boundary, 0 to 1 ms after the read.) A joystick entry has no guard: it cannot replace a key in the latch.
- **Joystick entries follow `delay_from` too.** Under `"read"` a joystick entry counts from the read of the most recent key before it, so a move timed against a game's clock stays with that clock as keys do.

**Ready input (2026-10-24):** a tester playing Beneath Apple Manor (Integer BASIC) lost keys to fixed delays in two ways: a move key sent while the game was still drawing was dropped (11 moves became 8), and a move onto the staircase square made the game ask a question, so the rest of the sequence became the answer. Delays tuned for one Dexterity or one screen cannot be right for both. The first idea, a key per keyboard read (`wait_for_read`), does not help, and a throwaway instrumented build (every `$C000`–`$C01F` access logged with its code address, stepped session, BAM level 1) showed why:
- **Read is not accepted.** Integer BASIC reads `$C000` at statement boundaries (the Ctrl-C check) and the idle loop. Every key was "read" within 0.5 ms of arriving, but the program only took it (cleared the strobe) 12 to 45 ms later; keys sent in between replaced it. A burst S,S,S moved once.
- **Screen-settled is not a signal.** After each move the program computes for about 1.7 s with the screen unchanged, then redraws once.
- **Where the program reads from is.** Waiting for a command it polled from two addresses about every 32 ms; computing a move, one of them went quiet; at the staircase prompt it switched to a new tight loop (about 67 reads per ms). The harness already wraps the `$C00x` read; it now also wraps `$C01x` (reads and writes) to see the strobe cleared, using AppleWin's own handler to decide (the strobe was set before and is clear after), and records the program counter of each read, which the tap already receives.
- **The rule** (`wait_for_ready`): ready = the program cleared the strobe after the previous key and has since read from every address it read from while the first key of the call waited to be taken (in the 300 ms before it was). Stop = before ready, 64 reads in a row from one address that was not an idle one, each under 100 cycles after the last. With it, one zero-delay call S,S,S,N,N,N,N,N made all six moves, each key going in under 1 ms after ready (about 2 s apart at D=20), and stopped when the staircase prompt appeared, before any stray key; the last two keys were reported unpressed. Answering the prompt (N, RETURN) in the next call worked, and so did moves sent straight after the answer.
- **Why the idle addresses come from the first key's wait.** The spike learned them from the reads in the 300 ms before the first key was pressed. That fails whenever a call starts while the program is changing state, which in a stepped session is every call made at once after the last: sent straight after answering a prompt, the prompt's loop was learned as idle and never polled again; sent straight after a stop, the busy-phase addresses from just before the prompt were; and moves sent straight after an answer learned addresses from the program's way out of the prompt. The loop that finds and takes the first key is, by definition, where the program waits for input now, so that is what the call learns. It includes the place that fetches the key once it is there: the //e ROM's input loop polls `$C28E` and reads the key again at `$C29B` before clearing the strobe, and `$C29B` never reads while the program waits, so the answer N, RETURN stalled after N. A tight loop at an idle place therefore also counts as ready, the mirror of the stop rule (a tight loop anywhere else).
- **Done when the last key is accepted.** Waiting for ready after the last key would never finish when that key leaves a prompt (the program does not return to its idle loop in the window), so the sequence completes once the program takes the last key.
- **`until_ready`, opt-in (2026-10-25).** With the call done at the take, a `look` came back before the move was drawn: after a single E the tester's screenshot still showed the old position, and after a run of moves they waited about 2.5 s before looking. A last key that brought up a prompt also went unreported, since no key was left to stop. `until_ready: true` makes the end wait for ready too and report how the program went on (`then`); the program has taken every key, so a prompt or 600 frames without ready completes the call rather than failing it. Opt-in, at the user's choice: the call returns a whole move later, and answering a prompt with it waits the full 600 frames (the case above). Ready is the input loop, not the screen: a program that draws after it starts polling again can still be caught a frame early.
- **The command loop is remembered (2026-10-25).** The tester found that answering the staircase prompt with `until_ready` ended `not_ready` after 10 s although the game took the next move at once, and that two zap batches in combat timed out the same way, each time with the next key read within 9 ms. A call learns its idle places from the loop that takes its first key; when that key goes into a prompt, they are the prompt's loop (`$C28E`, `$C29B`), and the game, back in its command loop (`$EEFB`, `$F035`), never reads from them again. A trace of 200 moves, wall messages and four prompts showed no other set of places, so the combat case is taken to be the same: a batch begun while a message or question waited for a key. (Not confirmed: the runs met no monster.) The fix keeps the places of the last call whose first key was not taken at a prompt, which in this game is every move, and counts reading from all of them as ready too. A key taken at a prompt is recognised by the tight loop that found it ending within 1 ms of the take. Prompt detection is unchanged: a move that brings up a prompt never reads from both command-loop places first. Kept until a reset or a snapshot load, since they belong to the program that was running. First version missed one case, found by the tester: in `N`, `RETURN`, `E`, `W`, `E` sent at the staircase prompt, the W stepped back onto the staircase and the last E answered the new prompt, because the prompt's loop was still an idle place of the call and a tight loop there counts as ready. Back in the command loop, the call now takes its places as its idle ones, so the second prompt stops it.
- **Not tried:** realtime (the same emulator code), Applesoft games (a prompt and a command wait may share a routine, so only the ready wait helps there), a program that never clears the strobe (Conan, `delay_from: "read"` remains the tool).
- **`type_text` already waits for the strobe**, so it never drops a move key; it cannot stop at a prompt and takes only printable characters and RETURN.
- **Exclusions chosen for simplicity:** no per-entry delays, joystick entries or `wait_for_read: false` with `wait_for_ready`. The idle addresses are the first key's reads in the 300 ms before it was taken; a program that took it without reading (a flush) gives none, and ready is then the next read after a key was taken.

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
