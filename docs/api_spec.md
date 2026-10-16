# AppleWin Harness — API & Protocol Specification

**Version:** 0.2 (draft). Design reasoning lives in [decisions.md](decisions.md).

## Overview
The harness lets clients drive an AppleWin emulator that it hosts in-process. The API is split by role (D4):

- **Agent API** — only what a person at the machine could do: type, move the joystick, look at the screen, and listen.
- **Operator API** — sessions, disks, execution control, checkpoints and test helpers. The test runner or experiment orchestrator uses it.

## Launching
```
AppleWinHarness.exe [--headless]
```

| Option | Default | Meaning |
|---|---|---|
| *(none)* | windowed | Opens a view-only window showing the live Apple screen and plays the Apple's sound (D8). Sound plays only in realtime mode. |
| `--headless` | off | No window and no sound. Use it for CI, batch runs, and machines without a desktop. |

- The display mode is fixed for the life of the process.
- The display mode never changes emulation results.
- In windowed mode, closing the window shuts the harness down. This works like `close_session` followed by exit.

## Transport (D3)
- **Pipe:** a Windows named pipe, `\\.\pipe\applewin-harness-<session-id>`. Its ACL allows only the user who launched the harness.
- **Messages:** JSON-RPC 2.0, encoded as UTF-8, with one JSON object per message. Each message is prefixed with its length as a 4-byte little-endian integer.
- **Notifications:** JSON-RPC notifications (no `id`) are used for events.
- **Frames:** raw frames can optionally be sent through shared memory (see below).

### Handshake
The first message on every connection must be `hello`:

```json
{ "jsonrpc": "2.0", "id": 1, "method": "hello",
  "params": { "protocol_version": "0.2", "role": "agent", "token": "<token>" } }
```

- The harness creates one token per role when it starts. It passes the tokens to whoever launched it, through stdout or an environment variable, and never writes them to disk.
- The response is `{ protocol_version, role, display_mode: "windowed" | "headless" }`.
- The role is fixed for the life of the connection.
- The server returns `-32001` if an agent connection calls an operator method.
- Requests after `hello` don't carry a token.

## Time (D10)
- **`emulated_cycles`** is the number of 6502 cycles since power-on.
- **`emulated_ms`** is `emulated_cycles` divided by the base clock: about 1.0205 MHz (NTSC, 60 Hz) or the PAL clock (50 Hz). It ignores the speed setting.
- **Every duration in the API is in emulated milliseconds.** This covers `hold_ms`, `inter_key_delay_ms` and `duration_ms`. As a result, a script gives the same results at any speed and in either run mode.
- **Every input response includes `applied_at_cycle`.** That's the emulated cycle at which the input took effect.

## Agent API

### Keyboard
The Apple II keyboard sends 7-bit ASCII and has no separate modifier keys, so keys are given as characters or named specials, not PC scan codes.

**`type_text`** `{ text: string, inter_key_delay_ms?: int = 30 }`
Sends each character as one keypress. Newlines (`\n`) become RETURN. Lowercase is sent as-is; on an Apple II/II+ model the harness converts it to uppercase.

**`press_key`** `{ key: string | int, hold_ms?: int = 50 }`
Presses a single key.
- `key` is either one character, an ASCII code from 0 to 127, or one of these names: `RETURN`, `ESC`, `TAB`, `DELETE`, `LEFT`, `RIGHT`, `UP`, `DOWN`, `CTRL-<letter>`, `RESET`.
- `RESET` sends CTRL-RESET.
- `hold_ms` affects the //e any-key-down flag ($C010) and auto-repeat.

The Open-Apple and Solid-Apple keys are joystick buttons 0 and 1; use `set_joystick` for them.

### Joystick
**`set_joystick`** `{ joystick?: 0|1 = 0, x?: 0..255, y?: 0..255, buttons?: { "0"?: bool, "1"?: bool, "2"?: bool }, duration_ms?: int }`
- These are the native paddle values; 127 is centre.
- Any field left out keeps its current value.
- If `duration_ms` is set, the joystick returns to its previous state after that long.

### Screen
**`get_frame`** `{ format?: "PNG" | "BGRA32" = "PNG", borders?: bool = false }`

Returns `{ frame_id, width, height, stride, format, data? }`.
- `data` is base64-encoded. It is left out if the session uses shared memory.
- `BGRA32` is AppleWin's own framebuffer format, so no conversion is needed.
- **Size without borders:** 560×384 for II-series video modes, where each 280×192 Apple pixel is drawn as 2×2. VidHD modes are larger.
- **Size with borders:** add 20 px on the left and right and 18 px on the top and bottom, giving 600×420.
- JPEG is deliberately not supported: lossy compression breaks pixel and checksum assertions.

**`subscribe_frames`** `{ every_n_frames?: int = 1 }`
After this call the server sends `frame_ready` notifications.

**`get_text_screen`** *(optional; only available if the session sets `allow_text_screen`)*
Returns `{ mode: "40col" | "80col" | "graphics", lines: string[24] }`.
- The text is decoded from video memory, which goes beyond strictly reading pixels. That's why it's opt-in.
- It returns `mode: "graphics"` with empty `lines` when the screen isn't in a text mode.

### Sound (D12)
The agent hears the same sound a person would: the speaker and Mockingboard, mixed together.
- It's captured in emulated time, in every mode, including headless and stepped.
- Playback volume and muting don't affect it.

**`get_audio`** `{ from_cycle?: int, duration_ms?: int = 1000, format?: "WAV" | "PCM16" = "WAV" }`

Returns `{ start_cycle, end_cycle, sample_rate: 44100, channels: 2, format, data }`.
- `data` is base64-encoded.
- If `from_cycle` is left out, the result is the most recent `duration_ms` of audio.
- `duration_ms` is capped at 30,000.
- Audio older than the 30-second ring buffer returns `-32009`.

**`subscribe_audio`** `{ threshold_db?: number = -50, min_silence_ms?: int = 100 }`

After this call the server sends `sound_started` and `sound_stopped` notifications.
- Sound "starts" when the level rises above `threshold_db`.
- It "stops" once it has stayed below the threshold for `min_silence_ms`.

### Agent notifications
- `frame_ready` `{ frame_id, emulated_ms, width, height, stride, format, shm_name? }`
- `sound_started` `{ start_cycle, emulated_ms }`
- `sound_stopped` `{ start_cycle, end_cycle, duration_ms, peak_db, dominant_hz }`. Fetch the actual samples with `get_audio { from_cycle: start_cycle }`.

## Operator API

### Sessions
**`create_session`** `{ model?: "apple2" | "apple2plus" | "apple2e" | "apple2ee" = "apple2ee", disks?: [ { slot: 6, drive: 1|2, path } ], hard_disks?: [ { slot: 7, unit: 1|2, path } ], run_mode?: "realtime" | "stepped" = "realtime", speed?: { video_refresh?: 50 | 60 = 60, ...set_speed params }, allow_text_screen?: bool = false, shared_memory_frames?: bool = false }`

Returns `{ session_id }`.
- The harness copies each image into a working directory and mounts the copies. The original files are never opened for writing (D5).
- In `stepped` mode, emulation only advances in response to `step_frames` or `step_cycles`.
- `speed.video_refresh` picks the base clock: 60 means NTSC, 50 means PAL. It is fixed for the session, because AppleWin restarts the emulator to change it. The other `speed` fields set the initial speed (see Speed below).

**`close_session`** `{ discard?: bool = true }` — stops the session and deletes its working copies. Use `export_disk` first to keep any results.

**`list_disks`** — returns `[ { slot, drive, original_path, working_path, write_protected, format } ]`

**`export_disk`** `{ slot, drive, dest_path }` — copies a working image out of the session. It refuses to overwrite the original image.

### Execution control
- **`pause`** — returns once the emulation loop has stopped at a frame boundary.
- **`resume`** — only valid in `realtime` mode. Realtime mode runs in 1 ms bursts of `effective_clock_hz × 1 ms` cycles, paced by a high-resolution timer (D9).
- **`step_frames`** `{ frames: int }` — runs N video frames (17,030 cycles each at 60 Hz), then pauses. Returns `{ emulated_cycles }`. Input queued while paused takes effect at the start of the next step, which keeps runs deterministic.
- **`step_cycles`** `{ cycles: int }` — *(planned for after M4)* runs N CPU cycles.

### Speed (D11)
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

### Checkpoints (D5)
- **`checkpoint`** `{ name }` — pauses the emulator if it's running, flushes the disk buffers, writes a save state, and copies the working disks. Returns `{ checkpoint_id }`.
- **`restore_checkpoint`** `{ checkpoint_id }` — restores the save state and the disks together, and leaves the emulator paused.
- **`list_checkpoints`** — returns `[ { checkpoint_id, name, created_at, emulated_ms } ]`
- **`delete_checkpoint`** `{ checkpoint_id }`

### Test helpers
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
| -32002 | No session |
| -32003 | Resource locked |
| -32004 | Checkpoint failed |
| -32005 | Disk not found |
| -32006 | Not supported (by this model or build) |
| -32007 | Busy, try again |
| -32008 | Invalid state (e.g. `resume` in stepped mode) |
| -32009 | Audio no longer buffered |

Error objects look like `{ code, message, data? }`.

## Limits
- Input rate: by default, at most 200 keyboard or joystick events per emulated second.
- Message size: 16 MiB per request or response.
- Paths: `dest_path` and disk paths must be absolute and must not point into the harness working directory.

## Example: the M2 scenario over the API
```text
operator → hello {role: "operator", ...}
operator → create_session {disks: [{slot: 6, drive: 1, path: "C:\\disks\\dos33.dsk"}], run_mode: "stepped"}
operator → step_frames {frames: 180}          # boot
agent    → hello {role: "agent", ...}
agent    → type_text {text: "PRINT 2+2\n"}
operator → step_frames {frames: 30}
agent    → get_frame {format: "PNG"}          # assert golden checksum
```
