# AppleWin Harness — API & Protocol Specification

**Version:** 0.3 (draft). M3 (server, sessions, input, frames, execution control, agent audio, speed controls) and M4 (checkpoints, state checksum, `step_cycles`, disks) are implemented. Design reasoning lives in [decisions.md](decisions.md).

## Overview
The harness lets clients drive an AppleWin emulator that it hosts in-process. The API is split by role (D4):

- **Agent API** — only what a person at the machine could do: type, move the joystick, look at the screen, and listen.
- **Operator API** — sessions, disks, execution control, checkpoints and test helpers. The test runner or experiment orchestrator uses it.

## Launching
```
AppleWinHarness.exe --serve [--headless]
```

| Option | Default | Meaning |
|---|---|---|
| `--serve` | off | Serves this API on a named pipe. It cannot be combined with `--realtime`, `--disk1`, `--png` or any step (usage error, exit 2). |
| *(no `--headless`)* | windowed | Opens a view-only window showing the live Apple screen and plays the Apple's sound (D8). Sound plays only in realtime mode. |
| `--headless` | off | No window and no sound. Use it for CI, batch runs, and machines without a desktop. |

- **Announcement.** On start the server prints exactly one line to stdout and flushes it. Nothing else is ever written to stdout:
  `{"pipe":"\\\\.\\pipe\\applewin-harness-<id>","agent_token":"<64 hex>","operator_token":"<64 hex>","pid":<n>}`
  - The keys are in that order. `<id>` is 32 random hex characters.
  - Each token is 32 random bytes from `BCryptGenRandom`, written as hex. They are never written anywhere else.
- **One session per process.** The process serves at most one session (`create_session` once). Start another process for another session.
- **Exit.** The process exits with 0 after `close_session` has been answered, or when the window is closed. It exits with 1 on a fatal error, after printing `error: …` to stderr. The process also exits with 0 when the process that launched it exits (an orphaned server never lingers); if the launcher cannot be watched at startup, it prints one line to stderr and carries on. Working copies and the INI file are removed on every exit path.
- The display mode is fixed for the life of the process, and it never changes emulation results.
- In windowed mode, closing the window shuts the harness down. This works like `close_session` followed by exit.

## Transport (D3)
- **Pipe:** a Windows named pipe, `\\.\pipe\applewin-harness-<id>`, in byte mode with overlapped I/O. It allows at most 8 simultaneous connections. Remote clients are rejected (`PIPE_REJECT_REMOTE_CLIENTS`).
- **ACL:** the DACL has exactly one ACE, an allow ACE for the SID of the user who launched the harness.
- **Messages:** JSON-RPC 2.0, encoded as UTF-8, with one JSON object per message. Each message is prefixed with its length as a 4-byte little-endian integer.
- **Framing:** a length of 0 or more than 16 MiB closes the connection. So does a connection that closes mid-message. The server keeps running.
- **Send backlog:** replies and notifications are queued per connection and never block emulation. If a connection's unsent backlog passes 64 MiB, the server closes it.
- **Notifications:** JSON-RPC notifications (no `id`) are used for events. Notifications sent by clients are ignored.
- **Frames:** raw frames through shared memory are planned (see below) and not yet available.

### Handshake
The first message on every connection must be `hello`:

```json
{ "jsonrpc": "2.0", "id": 1, "method": "hello",
  "params": { "protocol_version": "0.3", "role": "agent", "token": "<token>" } }
```

- The harness creates one token per role when it starts. It passes them to whoever launched it in the announcement line on stdout, and never writes them to disk.
- The response is `{ protocol_version: "0.3", role, display_mode: "windowed" | "headless" }`.
- A `protocol_version` other than `"0.3"` returns `-32600` and the server closes the connection.
- A wrong token or role, or any method other than `hello` as the first message, returns `-32001` and the server closes the connection. So does malformed JSON or an invalid request as the first message (the error reply is sent first).
- A connection that has not completed `hello` within 10 seconds is closed, which frees its slot.
- The role is fixed for the life of the connection.
- The server returns `-32001` if an agent connection calls an operator method. The connection stays open.
- Requests after `hello` don't carry a token.
- After `hello`, malformed JSON returns `-32700`, a message that isn't a JSON-RPC 2.0 request returns `-32600`, an unknown method returns `-32601`, and bad params return `-32602`. The connection stays open in all of these.

## Time (D10)
- **`emulated_cycles`** is the number of 6502 cycles since power-on.
- **`emulated_ms`** is `emulated_cycles` divided by the base clock: about 1.0205 MHz (NTSC, 60 Hz) or the PAL clock (50 Hz). It ignores the speed setting.
- **Every duration in the API is in emulated milliseconds.** This covers `hold_ms`, `inter_key_delay_ms` and `duration_ms`. As a result, input timing doesn't depend on the speed, and a stepped script gives the same results at any speed setting: stepped runs always use authentic speed and enhanced disk access, and speed settings affect realtime runs only.
- **Every input response includes `input_id` and `applied_at_cycle`.** `applied_at_cycle` is the emulated cycle at which the input will take effect, or `null` if that isn't known yet. The exact cycles arrive in an `input_applied` notification (see Input replies).

## Agent API
Before a session exists, every agent method returns `-32002`.

### Input replies
`type_text`, `press_key` and `set_joystick` reply straight away with `{ input_id, applied_at_cycle }`. They never wait for the input to take effect, because in stepped mode that only happens during a step, and a single-threaded client would deadlock.
- Input takes effect at the start of the next step (D10). `applied_at_cycle` is the cycle at which it will take effect when that is already known: nothing is queued ahead of it and, for a key, the keyboard strobe is clear. Then it equals the current `emulated_cycles`. Otherwise it is `null`.
- When an input has fully taken effect, that connection receives the notification `input_applied { input_id, first_cycle, last_cycle }`. For `type_text`, these are the cycles of the first and last key. For a held key or a joystick `duration_ms`, they are the press and the release.
- An input that `restore_checkpoint` throws away (still queued, or waiting for a release) is reported once as `input_applied { input_id, first_cycle: null, last_cycle: null, discarded: true }` (D1). The `discarded` field appears only then.
- Inputs are applied in order. A key also waits until the keyboard strobe is clear.
- Auto-repeat on held keys is not implemented.

### Keyboard
The Apple II keyboard sends 7-bit ASCII and has no separate modifier keys, so keys are given as characters or named specials, not PC scan codes.

**`type_text`** `{ text: string, inter_key_delay_ms?: int = 30 }`
Sends each character as one keypress. `text` may contain only characters 0x20 to 0x7E and `\n`, and is at most 4,096 characters; otherwise `-32602`. Newlines (`\n`) become RETURN. Lowercase is sent as-is (the //e is the only supported model).
- Key *i* is applied at the later of the previous key's applied cycle plus `inter_key_delay_ms` (at the base clock) and the moment the strobe clears. With `inter_key_delay_ms: 0` this is the fastest the keyboard can take the keys.

**`press_key`** `{ key: string | int, hold_ms?: int = 50 }`
Presses a single key.
- `key` is either one character from 0x20 to 0x7E, an ASCII code from 0 to 127, or one of these names: `RETURN` (0x0D), `ESC` (0x1B), `TAB` (0x09), `DELETE` (0x7F), `LEFT`, `RIGHT`, `UP`, `DOWN`, `CTRL-A` to `CTRL-Z` (0x01 to 0x1A), `RESET`. Anything else is `-32602`.
- `RESET` sends CTRL-RESET. It does not wait for the strobe.
- `hold_ms` sets how long the //e any-key-down flag ($C010 bit 7) stays set.
- There is no upper limit on `hold_ms` yet (planned for M6).

The Open-Apple and Solid-Apple keys are joystick buttons 0 and 1; use `set_joystick` for them.

### Joystick
**`set_joystick`** `{ joystick?: 0 = 0, x?: 0..255, y?: 0..255, buttons?: { "0"?: bool, "1"?: bool }, duration_ms?: int }`
- These are the native paddle values; 127 is centre. Positions start at 127.
- Any field left out keeps its current value.
- If `duration_ms` is set, the joystick returns to its previous state after that long. There is no upper limit yet (planned for M6).
- `joystick: 1` and button `"2"` return `-32006` (not yet supported).

### Screen
**`get_frame`** `{ format?: "PNG" | "BGRA32" = "PNG", borders?: bool = false }`

Returns `{ frame_id, emulated_cycles, width, height, stride, format, data }`.
- `data` is base64-encoded (RFC 4648, with padding).
- The frame is the last presented frame. `frame_id` is the number of frames presented so far, and `emulated_cycles` is the time of the call.
- `BGRA32` is AppleWin's own framebuffer format. The rows are top-down (top row first) and `stride = width × 4`.
- `PNG` reports `stride = width × 3`.
- **Size without borders:** 560×384 for II-series video modes, where each 280×192 Apple pixel is drawn as 2×2. VidHD modes are larger.
- **Size with borders:** add 20 px on the left and right and 18 px on the top and bottom, giving 600×420.
- JPEG is deliberately not supported: lossy compression breaks pixel and checksum assertions.

**`subscribe_frames`** `{ every_n_frames?: int = 1 }`
- `every_n_frames` is 1 to 600. After this call, the connection receives `frame_ready { frame_id, emulated_cycles, width, height }` after every *n*-th presented frame.
- Counting starts at the subscribe call. Calling it again replaces the setting and restarts the count. `every_n_frames: 0` unsubscribes.

**`get_text_screen`** `{}`
Returns `{ mode: "40col" | "80col" | "mixed40" | "mixed80" | "graphics", lines: string[24], emulated_cycles }`.
- Agent method. It needs a session created with `allow_text_screen: true`; otherwise it returns `-32006`. The text is decoded from video memory, which goes beyond strictly reading pixels. That's why it's opt-in.
- **Mode and page:** decoded from the video soft switches (TEXT, MIXED, PAGE2, 80COL, 80STORE, ALTCHARSET). `mode` is `40col` or `80col` for full-screen text, `mixed40` or `mixed80` for graphics with 4 text lines, and `graphics` for full graphics. With 80STORE on, PAGE2 selects memory banks, not the displayed page, so the display stays on page 1. 80-column lines interleave auxiliary (even columns) and main (odd columns) memory.
- **Lines:** always 24. Text lines keep their trailing spaces, so each is exactly 40 characters (`40col`, `mixed40`) or 80 (`80col`, `mixed80`). In the mixed modes lines 0-19 are `""` and lines 20-23 are text. In `graphics`, all 24 lines are `""`.
- **Character mapping:** every character becomes printable ASCII 0x20-0x7E.
  - Inverse and flashing characters map to their base character, so the same letters come back whether they are inverse, flashing or normal.
  - Normal-video codes $E0-$FF map to lowercase `a`-`z` (and `{|}~`), as on the //e. Normal $A0-$DF map to themselves.
  - MouseText (in the alternate character set) and DEL map to a space, as does anything else outside 0x20-0x7E.

### Sound (D12, D14)
The agent hears the same sound a person would: the speaker and Mockingboard, mixed together.
- It's captured in emulated time, in every mode, including headless and stepped.
- Playback volume and muting don't affect it.

**`get_audio`** `{ from_cycle?: int, duration_ms?: int = 1000, format?: "WAV" | "PCM16" = "WAV" }`

Returns `{ start_cycle, end_cycle, sample_rate: 44100, channels: 2, format, data }`.
- `data` is base64. `WAV` is a canonical 44-byte-header WAV; `PCM16` is raw little-endian int16 interleaved samples.
- `duration_ms` must be 1 to 30,000 (`-32602` otherwise).
- If `from_cycle` is left out, the result is the most recent `duration_ms` of audio.
- A range that starts before the oldest sample in the 30-second ring buffer returns `-32009`.
- A range that extends past the newest sample is cut short; `end_cycle` reports the actual end.

**`subscribe_audio`** `{ threshold_db?: number = -50, min_silence_ms?: int = 100 }`

`threshold_db` must be a number in -120..0 and `min_silence_ms` an integer in 10..10000 (`-32602` otherwise). After this call the server sends `sound_started` and `sound_stopped` notifications.
- The level is the AC RMS in dBFS of each 10 ms block, after removing the block's mean and linear trend.
- Sound "starts" at the first block above `threshold_db`.
- It "stops" once it has stayed below the threshold for `min_silence_ms`. `end_cycle` is the end of the last loud block.
- Events are produced 110 ms of emulated time behind the newest audio, so a trailing sound's `sound_stopped` arrives once emulation has advanced that far.
- Measured example: the //e bell (`PRINT CHR$(7)`) has a dominant frequency of about 934.5 Hz and lasts about 120 ms by the detector (the ROM's tone timing is about 103 ms).

### Agent notifications
- `frame_ready` `{ frame_id, emulated_cycles, width, height }` (shared-memory fields are planned)
- `input_applied` `{ input_id, first_cycle, last_cycle, discarded? }` (`discarded: true` with null cycles when `restore_checkpoint` dropped the input)
- `sound_started` `{ start_cycle, emulated_ms }` (`emulated_ms` is milliseconds of emulated time since power-on)
- `sound_stopped` `{ start_cycle, end_cycle, duration_ms, peak_db, dominant_hz }` (`peak_db` in dBFS, `dominant_hz` in Hz). Fetch the actual samples with `get_audio { from_cycle: start_cycle }`.

## Operator API

### Sessions
**`create_session`** `{ model?: "apple2ee" = "apple2ee", disks?: [ { slot: 6, drive: 1|2, path } ], run_mode?: "realtime" | "stepped" = "realtime", speed?: {...}, allow_text_screen?: bool = false, shared_memory_frames?: bool = false }`

Returns `{ session_id }`.
- `path` must be absolute (`-32602`). A missing file returns `-32005`.
- The harness copies each image into a working directory (`%TEMP%\applewin-harness\<pid>\`) and mounts the copies. The original files are never opened for writing (D5).
- A realtime session starts running. A stepped session starts paused, and emulation only advances in response to `step_frames`.
- A second `create_session` returns `-32008`: there is one session per process.
- **Not supported yet (`-32006`):** models other than `apple2ee`, `hard_disks`, `shared_memory_frames: true`, and `speed.video_refresh: 50` (PAL).
- `speed` is `{ video_refresh?: 50 | 60 = 60, ...set_speed params }`. It is validated before anything starts (`-32602`), and applied right after the emulator starts. `video_refresh: 50` returns `-32006`.

**`close_session`** `{}` — replies `{}`, then stops the session, deletes its working copies and checkpoints and exits the process with code 0. It is also valid when no session exists, and when the session is unusable. Use `export_disk` first to keep any results.

**`list_disks`** `{}` — returns `[ { slot: 6, drive: 1|2, original_path, working_path, write_protected, format } ]`, with empty drives left out. `format` is the image type AppleWin reports (`"DO"`, `"PO"`, `"NIB"`, `"WOZ"`, ...). Before a session exists: `-32002`.

**`export_disk`** `{ slot: 6, drive: 1|2, dest_path }` — flushes the drive's current track, copies the working image to `dest_path` and replies `{}`.
- `dest_path` must be absolute, must not exist, must not be any session's original image, must not be inside the harness work directory (`%TEMP%\applewin-harness`), and its directory must exist; otherwise `-32602`. It must also name an ordinary file: no `\\?\` or `\\.\` prefix, no `:` after the drive letter (an NTFS alternate data stream), and no Windows device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`, `LPT1`-`LPT9`, `CONIN$`, `CONOUT$`, with or without an extension) in any path segment; otherwise `-32602`. Original images are never touched.
- An empty drive is `-32005`. Any slot other than 6 is `-32602`. After a failed restore it returns `-32008`.

**`insert_disk`** `{ slot: 6, drive: 1|2, path, write_protected?: bool = false }` — puts a floppy in the drive and replies `{}`.
- `path` must be absolute (`-32602`), and a missing file is `-32005`. Any slot other than 6 is `-32602`.
- Like `create_session`, it copies the image into the work directory and mounts the copy. The original is never opened for writing (D5). Each insert gets a working copy of its own (`%TEMP%pplewin-harness\<pid>\insert-<n>\`), so the same image inserted twice, or in both drives, never collides.
- A disk already in the drive is flushed and ejected first. Its working copy is kept until `close_session`.
- It works in both run modes and takes effect between frames, on the emulator thread; it doesn't pause or reset the machine, and in a stepped session no time passes.
- An image AppleWin can't mount (for example a file whose format is not recognized) is `-32005` with AppleWin's reason, and the drive is left as it was. Putting the previous disk back may reset the drive's rotational position.

**`eject_disk`** `{ slot: 6, drive: 1|2 }` — flushes the drive's current track, unmounts it and replies `{}`. An empty drive is `-32005`. Any slot other than 6 is `-32602`. The working copy is kept until `close_session`.

Both: after a failed restore they return `-32008`. Before a session exists they return `-32002`. `list_disks` and `export_disk` reflect every swap. `checkpoint` records the drives as they are, including empty ones, and `restore_checkpoint` puts back the checkpointed drive contents (a disk swapped in or out since, or an empty drive, included).

### Execution control
Before a session exists, these return `-32002`.
- **`pause`** `{}` — in realtime mode it runs until the next present, then pauses, and replies `{ emulated_cycles }`. If already paused, it replies at once.
- **`resume`** `{}` — only valid in `realtime` mode (`-32008` in stepped mode). Replies `{}`. Realtime mode runs in 1 ms bursts, paced by a high-resolution timer (D9).
- **`step_frames`** `{ frames: int }` — `frames` is 1 to 100000. Pauses first if the session is running, runs N video frames (17,030 cycles each at 60 Hz), then stays paused. Returns `{ emulated_cycles }`. Input queued while paused takes effect at the start of the next step, which keeps runs deterministic.
- If the window is closed during `pause` or `step_frames`, the reply is `-32008` and the process then exits with 0.
- **`step_cycles`** `{ cycles: int }` — `cycles` is 1 to 17,030,000. Like `step_frames` it pauses first if the session is running, runs in stepped mode (the last instruction may overshoot, by at most 7 cycles), and replies `{ emulated_cycles }`. Input, `input_applied` and sound events behave as for `step_frames`. Replays must use the same sequence of run commands: audio capture depends on how a run is split into steps (ruling B13).

### Speed (D11)
These commands offer exactly the speed controls AppleWin has, with AppleWin's value ranges. The harness implements them by calling AppleWin's own functions (`SetCurrentCLK6502`, `UseClockMultiplier`, `SetEnhanceDisk`), so clamping and rounding match AppleWin exactly.

**`set_speed`** `{ machine_speed?: 0..40 | "authentic" | "max", clock_multiplier?: number, enhance_disk_speed?: bool, full_speed_override?: bool }`

Any field left out keeps its current value. Returns the same result as `get_speed`. These are operator methods: `-32001` for an agent connection, `-32002` before a session. `video_refresh` is not accepted here (`-32602`); it is set only in `create_session`.

| Field | AppleWin equivalent | Meaning |
|---|---|---|
| `machine_speed` | Config dialog speed slider | `"authentic"` is the same as 10. `"max"` is the same as 40. 0–9 give a multiplier of `0.5 + 0.05 × value`; 10–39 give `value / 10`; 40 runs unthrottled. |
| `clock_multiplier` | `-clock-multiplier` | Converted to a slider value: below 0.5 is raised to 0.5; below 1 gives `(int)((x−0.5)×20)`; otherwise `(int)(x×10)`, capped at 39. It can't select unthrottled speed. Sending it together with `machine_speed` is an invalid-params error (`-32602`). |
| `enhance_disk_speed` | Config ▸ "Enhance disk access speed" (default `true`) | Runs at full speed while a Disk II motor is on, provided the speaker and Mockingboard are silent. An empty drive keeps its motor on, so with this setting a realtime session with no disk runs at full speed (AppleWin's own rule). Set it to `false` to keep authentic pacing. Realtime runs only: stepped runs always use enhanced disk access. |
| `full_speed_override` | Holding or toggling Scroll Lock | Forces full speed while it's on. |

**`get_speed`** returns:

```
{
  machine_speed,
  clock_multiplier,            // effective_clock_hz / base_clock_hz; null when machine_speed is 40
  video_refresh,
  base_clock_hz,
  effective_clock_hz,          // null when machine_speed is 40
  enhance_disk_speed,
  full_speed_override,
  full_speed_active,           // true only while a realtime run is actually stepping at full speed
  full_speed_reasons           // array of: "machine_speed_max", "override", "disk_motor"
}
```

**Rules:**
- **Stepped runs always use authentic speed and enhanced disk access; speed settings affect realtime runs only (D9, ruling C7).** In stepped mode the settings are stored and reported, and they can't change emulated results: frames, CPU state and captured audio are the same at every setting. (In a realtime run, `enhance_disk_speed` also changes AppleWin's disk timing, and the speed changes how many samples AppleWin's sound devices produce per cycle.)
- **At full speed in realtime mode**, frames are redrawn the way AppleWin does it, and `frame_ready` notifications are limited to one per 16 ms of wall-clock time per subscription (about 62.5 per second). Extra presents are dropped; `frame_id` stays monotonic, so ids can skip. Expect one extra `frame_ready` when leaving full speed.
- **Windowed playback follows speed.** The window plays sound only at `machine_speed` 10 and not at full speed. Agent audio capture (`get_audio`, `subscribe_audio`) is unaffected, but silent while a run is at full speed.
- **Speed is a session setting.** `restore_checkpoint` doesn't change it: the session's current settings are re-applied after the load, and the speed stored with a checkpoint is informational only (D8).

### Checkpoints (D5)
A checkpoint is an AppleWin save state plus copies of the working disks, stored under `<work>/checkpoints/<checkpoint_id>/`. Checkpoints live and die with the session's work directory: they do not survive the process.

**`checkpoint`** `{ name?: string }` — returns `{ checkpoint_id, emulated_cycles }`.
- If the session is running, it first pauses at the next present and ends the realtime run (full speed and the Mockingboard mute are cleared). It then writes the save state, flushes the disk buffers and copies the working disks. The session is left paused.
- `checkpoint_id` is `cp-<n>`, counting up from 1. `name` is up to 64 printable characters (`-32602` otherwise); `list_checkpoints` reports `null` if it was omitted.
- Errors: `-32004` with the cause on any failure, and when there are already 256 checkpoints.
- **Non-ASCII working paths (D10):** AppleWin stores image paths as UTF-8 but loads them through the ANSI file API. If a working copy's path would not survive that round trip (for example a `%TEMP%` path with non-ASCII characters), `checkpoint` returns `-32004` and writes nothing.

**`restore_checkpoint`** `{ checkpoint_id }` — returns `{ emulated_cycles }`. It restores the save state and the disks together, and leaves the session paused in both run modes (use `resume` to start a realtime run). An unknown id is `-32602`.
- After a restore, everything observable depends only on the checkpoint (see D5), except the frame id counter (below): the machine, the disks, the video scanner position, the speaker, the random stream and the harness's own queues. Running the same script from the same checkpoint always gives the same state checksum, frames and audio, whatever happened before the restore.
- `emulated_cycles` continues from the checkpoint's value. Input still queued is discarded and reported with `discarded: true` (see Input replies), held keys and joystick buttons are released, and the joystick returns to the position it had at the checkpoint.
- The audio capture is cleared: `get_audio` for audio from before the restore returns `-32009`. `subscribe_audio` detectors restart at the restored cycle. `get_frame` straight after a restore shows the restored screen.
- The session's current speed settings are kept (D8).
- The drives end up as they were at the checkpoint, even if `insert_disk` or `eject_disk` changed them since: a disk inserted since is ejected, a drive that was empty is empty again, and a disk that was replaced or ejected comes back (its working copy is kept until `close_session`). Each checkpointed disk's contents are those of the checkpoint.
- Frame ids and `frame_ready` cadence continue across a restore; they are not reset. `frame_id` only counts up: the re-rendered frame straight after a restore keeps the last id (its pixels are the restored screen), no `frame_ready` is sent for it, and the `every_n_frames` phase carries on from before the restore.
- AppleWin looks each disk image up by its bare file name in the process's current directory before its absolute path. The harness makes the checkpoint's own directory current for the load and puts the previous one back, so the server's working directory may hold files of any name.
- **Failure before the load** (for example a missing checkpoint disk copy): `-32004`. The session stays usable and unchanged: `get_state_checksum` equals its value before the call (D7). Every check that does not need the drives empty (the save state, the disk copies, the image names) runs before the drives are ejected, so those failures leave the drives untouched. A failure after ejecting (a disk copy that can't be read or written) rolls the working disks back and re-inserts them; such a failure may reset the drive's rotational position. If the working disks can't be put back or re-inserted, the reply is still `-32004` and the session is then unusable, as below.
- **Failure in or after the load:** `-32004`, and the session is then unusable. `resume`, `step_frames`, `step_cycles`, `type_text`, `press_key`, `set_joystick`, `set_speed`, `checkpoint`, `restore_checkpoint`, `get_state_checksum`, `export_disk`, `insert_disk` and `eject_disk` return `-32008` until `close_session`. The other calls (`pause`, `get_frame`, `get_audio`, `get_speed`, `list_disks`, `list_checkpoints`, `delete_checkpoint`, `close_session`) still work.

**`list_checkpoints`** `{}` — returns `[ { checkpoint_id, name, created_at, emulated_cycles } ]`, oldest first. `created_at` is ISO 8601 UTC; `name` may be `null`.

**`delete_checkpoint`** `{ checkpoint_id }` — returns `{}`. An unknown id is `-32602`.

**Startup sweep (D11).** A killed server cannot clean up after itself. On start, the harness removes stale `%TEMP%\applewin-harness\<pid>` directories and `<pid>.ini` files whose process is no longer running. A live process's entries are never touched, and junctions and symbolic links are skipped, never followed.

### Test helpers
**`get_state_checksum`** `{}` — returns `{ sha256, emulated_cycles }`. It is an operator method.

`sha256` is the lowercase hex SHA-256 of the byte string below, little-endian throughout. It never includes wall-clock time, paths or harness bookkeeping. It is computed at any time. After a restore it equals the value when the checkpoint was made, except for input state the restore releases: keys and joystick buttons held at the checkpoint and timed joystick moves still pending then (the //e any-key-down flag, button and paddle bytes). Two restores of the same checkpoint always give the same value.

| # | Size in bytes | Content |
|---|---|---|
| 1 | 1+1+1+1+2+2 | CPU registers A, X, Y, P, then S and PC as 16-bit values (S is AppleWin's 16-bit `0x01xx` stack pointer) |
| 2 | 4 | the memory mode (`GetMemMode()`), uint32 |
| 3 | 65,536 | main RAM, including the language-card banks, taken as the save state takes it (dirty shadow pages flushed first) |
| 4 | 1 | the auxiliary card type (AppleWin's `SS_CARDTYPE` value) |
| 5 | varies | the aux RAM as the save state takes it: for an 80-column card the text page ($400-$7FF of the aux bank, 1,024 bytes); for the Extended 80-column card the 64 KiB aux bank; for a RamWorks III every bank; nothing if there is no aux card |
| 6 | 1 | the keyboard latch and strobe (`KeybReadData()`) |
| 7 | 4+1 | the video soft-switch mode (uint32), then the alternate-charset switch (0 or 1) |
| 8 | 1+1+2×(4+4), or 1 | the Disk II controller in slot 6: motor (0 or 1), current drive (0 or 1), then for drive 1 and drive 2 the phase (the bit pattern of a 32-bit float) and the track (uint32). If slot 6 has no Disk II, the single byte `0xFF` |
| 9 | 1+1+1+1+1 | input state (D6): the //e any-key-down flag ($C010 bit 7), joystick buttons 0 and 1, paddle positions 0 (x) and 1 (y) |

### Operator notifications
- `disk_written` `{ slot, drive }`
- `log` `{ level, message, emulated_ms }`

## Shared-memory frames
This applies when the session sets `shared_memory_frames: true`.

- **Name:** `Local\AppleWinFrame_<session-id>`
- **Header** (64 bytes, little-endian): `uint32 magic = 0x4E505741 ("AWPN")`, `uint32 seq`, `uint32 width`, `uint32 height`, `uint32 stride`, `uint32 format` (`BGRA32 = 1`), `uint64 frame_id`, `uint64 emulated_ms`, then padding to 64 bytes.
- **Pixels** start at byte 64, top row first.
- **Seqlock:** the writer increments `seq` to an odd value, writes the header and pixels, then increments `seq` to an even value. The reader:
  1. Reads `seq`.
  2. Retries if `seq` is odd.
  3. Copies the frame.
  4. Re-reads `seq`, and retries if it changed.

## Errors
The standard JSON-RPC codes apply: `-32700` parse error, `-32600` invalid request, `-32601` method not found, `-32602` invalid params, `-32603` internal error.

Harness-specific codes:

| Code | Meaning |
|---|---|
| -32000 | Generic harness error |
| -32001 | Unauthorized: bad token, or method not allowed for this role |
| -32002 | No session (agent or operator method that needs one, before `create_session`) |
| -32003 | Resource locked |
| -32004 | Checkpoint or restore failed |
| -32005 | Disk not found |
| -32006 | Not supported (by this model or build, or not yet implemented) |
| -32007 | Busy, try again |
| -32008 | Invalid state (e.g. `resume` in stepped mode, a second `create_session`, window closed during a step, run/input/checkpoint/export calls after a failed restore) |
| -32009 | Audio no longer buffered |

Error objects look like `{ code, message, data? }`. `-32603` carries the C++ exception's `what()` in `message`.

## Limits
- Message size: 16 MiB per request or response (framing). A larger length prefix closes the connection.
- Send backlog: 64 MiB of unsent data per connection; beyond that the server closes it.
- Connections: at most 8 at once.
- Not limited yet (M6): input rate (planned: 200 events per emulated second), `inter_key_delay_ms`, `hold_ms` and `duration_ms` values, and request sizes below 16 MiB. A client can therefore schedule events arbitrarily far ahead.
- Paths: disk paths must be absolute.

## Example: the M2 scenario over the API
```text
operator → hello {protocol_version: "0.3", role: "operator", ...}
operator → create_session {disks: [{slot: 6, drive: 1, path: "C:\\disks\\dos33.dsk"}], run_mode: "stepped"}
operator → step_frames {frames: 720}          # boot
agent    → hello {role: "agent", ...}
agent    → type_text {text: "PRINT 2+2\n", inter_key_delay_ms: 0}
operator → step_frames {frames: 60}
agent    → get_frame {format: "PNG"}          # assert golden checksum
```
