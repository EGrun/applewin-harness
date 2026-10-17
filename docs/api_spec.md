# AppleWin Harness — API & Protocol Specification

**Version:** 0.3 (draft). Parts A and B of M3 (server, sessions, input, frames, execution control, agent audio) are implemented. Sections marked "M3 Part C — pending" describe features that are not built yet. Design reasoning lives in [decisions.md](decisions.md).

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
- **Every duration in the API is in emulated milliseconds.** This covers `hold_ms`, `inter_key_delay_ms` and `duration_ms`. As a result, a script gives the same results at any speed and in either run mode.
- **Every input response includes `input_id` and `applied_at_cycle`.** `applied_at_cycle` is the emulated cycle at which the input will take effect, or `null` if that isn't known yet. The exact cycles arrive in an `input_applied` notification (see Input replies).

## Agent API
Before a session exists, every agent method returns `-32002`.

### Input replies
`type_text`, `press_key` and `set_joystick` reply straight away with `{ input_id, applied_at_cycle }`. They never wait for the input to take effect, because in stepped mode that only happens during a step, and a single-threaded client would deadlock.
- Input takes effect at the start of the next step (D10). `applied_at_cycle` is the cycle at which it will take effect when that is already known: nothing is queued ahead of it and, for a key, the keyboard strobe is clear. Then it equals the current `emulated_cycles`. Otherwise it is `null`.
- When an input has fully taken effect, that connection receives the notification `input_applied { input_id, first_cycle, last_cycle }`. For `type_text`, these are the cycles of the first and last key. For a held key or a joystick `duration_ms`, they are the press and the release.
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

**`get_text_screen`** *(planned for M5; returns `-32006` for now)*
Returns `{ mode: "40col" | "80col" | "graphics", lines: string[24] }`.
- The text is decoded from video memory, which goes beyond strictly reading pixels. That's why it's opt-in (`allow_text_screen`).
- It returns `mode: "graphics"` with empty `lines` when the screen isn't in a text mode.

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
- `input_applied` `{ input_id, first_cycle, last_cycle }`
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
- **Not supported yet (`-32006`):** models other than `apple2ee`, `hard_disks`, `allow_text_screen: true`, `shared_memory_frames: true`, `speed.video_refresh: 50` (PAL), and any `speed` field until Part C. Planned shape for `speed`: `{ video_refresh?: 50 | 60 = 60, ...set_speed params }` (M3 Part C — pending).

**`close_session`** `{}` — replies `{}`, then stops the session, deletes its working copies and exits the process with code 0. It is also valid when no session exists. Use `export_disk` first to keep any results (M4).

**`list_disks`** *(M4)* — returns `[ { slot, drive, original_path, working_path, write_protected, format } ]`

**`export_disk`** *(M4)* `{ slot, drive, dest_path }` — copies a working image out of the session. It refuses to overwrite the original image.

### Execution control
Before a session exists, these return `-32002`.
- **`pause`** `{}` — in realtime mode it runs until the next present, then pauses, and replies `{ emulated_cycles }`. If already paused, it replies at once.
- **`resume`** `{}` — only valid in `realtime` mode (`-32008` in stepped mode). Replies `{}`. Realtime mode runs in 1 ms bursts, paced by a high-resolution timer (D9).
- **`step_frames`** `{ frames: int }` — `frames` is 1 to 100000. Pauses first if the session is running, runs N video frames (17,030 cycles each at 60 Hz), then stays paused. Returns `{ emulated_cycles }`. Input queued while paused takes effect at the start of the next step, which keeps runs deterministic.
- If the window is closed during `pause` or `step_frames`, the reply is `-32008` and the process then exits with 0.
- **`step_cycles`** `{ cycles: int }` — *(planned for M4)* runs N CPU cycles.

### Speed (D11) — M3 Part C — pending
These commands offer exactly the speed controls AppleWin has, with AppleWin's value ranges. The harness implements them by calling AppleWin's own functions (`SetCurrentCLK6502`, `UseClockMultiplier`, `SetEnhanceDisk`), so clamping and rounding match AppleWin exactly.

**`set_speed`** `{ machine_speed?: 0..40 | "authentic" | "max", clock_multiplier?: number, enhance_disk_speed?: bool, full_speed_override?: bool }`

Any field left out keeps its current value. Returns the same result as `get_speed`.

| Field | AppleWin equivalent | Meaning |
|---|---|---|
| `machine_speed` | Config dialog speed slider | `"authentic"` is the same as 10. `"max"` is the same as 40. 0–9 give a multiplier of `0.5 + 0.05 × value`; 10–39 give `value / 10`; 40 runs unthrottled. |
| `clock_multiplier` | `-clock-multiplier` | Converted to a slider value: below 0.5 is raised to 0.5; below 1 gives `(int)((x−0.5)×20)`; otherwise `(int)(x×10)`, capped at 39. It can't select unthrottled speed. Sending it together with `machine_speed` is an invalid-params error (`-32602`). |
| `enhance_disk_speed` | Config ▸ "Enhance disk access speed" (default `true`) | Runs at full speed while a Disk II motor is on, provided the speaker and Mockingboard are silent. |
| `full_speed_override` | Holding or toggling Scroll Lock | Forces full speed while it's on. |

**`get_speed`** returns:

```
{
  machine_speed,
  clock_multiplier,
  video_refresh,
  base_clock_hz,
  effective_clock_hz,          // null when machine_speed is 40
  enhance_disk_speed,
  full_speed_override,
  full_speed_active,
  full_speed_reasons           // array of: "machine_speed_max", "override", "disk_motor"
}
```

**Rules:**
- **Speed settings only affect realtime pacing (D9).** In stepped mode they are stored and reported but can't change emulated results.
- **At full speed in realtime mode**, frames are redrawn the way AppleWin does it, and `frame_ready` notifications are limited to 60 per wall-clock second.
- **Speed is a session setting.** `restore_checkpoint` doesn't change it.

### Checkpoints (D5) — M4
- **`checkpoint`** `{ name }` — pauses the emulator if it's running, flushes the disk buffers, writes a save state, and copies the working disks. Returns `{ checkpoint_id }`.
- **`restore_checkpoint`** `{ checkpoint_id }` — restores the save state and the disks together, and leaves the emulator paused.
- **`list_checkpoints`** — returns `[ { checkpoint_id, name, created_at, emulated_ms } ]`
- **`delete_checkpoint`** `{ checkpoint_id }`

### Test helpers (M4)
**`get_state_checksum`** — returns `{ sha256 }`, a hash of main and aux RAM, the CPU registers and the memory soft-switch state. The exact list of fields is fixed during M4 and recorded here. Wall-clock time is never included.

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
| -32004 | Checkpoint failed |
| -32005 | Disk not found |
| -32006 | Not supported (by this model or build, or not yet implemented) |
| -32007 | Busy, try again |
| -32008 | Invalid state (e.g. `resume` in stepped mode, a second `create_session`, window closed during a step) |
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
