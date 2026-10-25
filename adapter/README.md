# applewin_mcp: an MCP server for the Apple II harness

`applewin_mcp` lets an LLM agent use an emulated Apple //e through AppleWin Harness. It is an MCP server over stdio. It offers only what a person at the machine could do: type, press keys, move the joystick, look at the screen, listen, let time pass, swap floppy disks, and save a floppy to an image file. Other operator concerns (the disk loaded at launch, the run mode, checkpoints) are not tools; they are set at launch (decision D4 in [docs/decisions.md](../docs/decisions.md)). The spec is [docs/specs/m5.md](../docs/specs/m5.md).

## Install
You need Python 3.12 or later and a built `AppleWinHarness.exe` (see the [main README](../README.md)).

```
pip install -r adapter/requirements.txt
```

This installs the official MCP Python SDK (`mcp==2.3.0`), the only dependency.

## Launch
Run it from the `adapter` directory, or put that directory on `PYTHONPATH`:

```
cd adapter
python -m applewin_mcp --exe <path\to\AppleWinHarness.exe> [--headless] [--disk <image>] [--realtime [--speed <0..40|authentic|max>] [--no-disk-boost]] [--text-screen] [--log-format text|json]
```

| Flag | Meaning |
|---|---|
| `--exe <path>` | Required. The harness executable. |
| `--headless` | No window and no audio device. Without it the harness shows a view-only window and plays the sound. |
| `--disk <image>` | A disk image for slot 6, drive 1. The harness works on a copy; the original is never written to. Without it the machine starts with no disk. |
| `--realtime` | Run in realtime mode: the Apple runs on the wall clock and never waits for the agent. The default is stepped mode. |
| `--speed <0..40\|authentic\|max>` | Realtime only. The harness's `machine_speed`: `authentic` (10) is the real 1.02 MHz, 20 is twice that, 40 or `max` is unthrottled. Without it the session keeps AppleWin's default (authentic). |
| `--no-disk-boost` | Realtime only. Turns off `enhance_disk_speed`, so the speed stays constant while a drive is running. By default AppleWin runs at full speed whenever a drive motor is on (and always, with no disk inserted). |
| `--log-format text\|json` | Format of the log lines on stderr (default `text`). It is also passed to the harness, whose stderr lines are forwarded unchanged, so in `json` mode every stderr line is one JSON object `{ts, level, msg, component, ...}` (`component` is `adapter` for the adapter's own lines). stdout is always MCP only. |
| `--version` | Prints the adapter version and exits. |
| `--text-screen` | Turn on the `read_screen` tool (the session is created with `allow_text_screen`). |

The server starts the harness (`--serve`), creates the session and serves MCP on stdio. stdout carries MCP frames only; log lines go to stderr. When the server exits it closes the session and the harness exits. To run a task, see [examples/basic_count.md](examples/basic_count.md).

## Tools
| Tool | Parameters | What it does |
|---|---|---|
| `type_text` | `text`, `inter_key_delay_ms?` | Types on the keyboard (`\n` is RETURN; printable ASCII, up to 4096 characters). |
| `press_key` | `key`, `hold_ms?` | Presses one key: a character, an ASCII code, or a name such as RETURN, ESC, TAB, DELETE, LEFT, RIGHT, UP, DOWN, CTRL-A to CTRL-Z, or RESET. |
| `press_keys` | `keys` (list of keys, `{key, delay_ms?, hold_ms?, wait_for_read?}`, or `{joystick: {x?, y?, buttons?}, delay_ms?, duration_ms?}`), `look?`, `frames?`, `every_ms?`, `region?`, `zoom?`, `delay_from?` (`applied` or `read`, default `applied`; also per entry), `wait_for_ready?` (default false), `until_ready?` (default false; with `wait_for_ready`) | Presses a timed key sequence in one call, in emulated time: each key `delay_ms` after the previous one and after the program has read it (unless `wait_for_read` is false), held `hold_ms`. A joystick entry moves the joystick for `duration_ms`, timed from the previous entry and never waiting for a read. Returns each entry's applied time and, for a key, its read time and latency (read minus applied): a program acts on a key when it reads it, which for a slowly polling program can be a whole loop period after the key went in. With `delay_from` `read`, `delay_ms` counts from the program's read of the key before it instead of from that key's apply, so the keys keep the same timing relative to the program in every run and in stepped and realtime alike; use it for timings tuned to a game's own clock (a joystick entry then also counts from the read of the key before it). Entries go in on their exact cycle; a key waiting for a read goes in at least 1 ms after it. With `wait_for_ready` true each key is sent only when the program is waiting for its next command again, instead of after a guessed delay: for a run of moves in a turn-based game, where a move takes longer the more the game has to do and a key sent meanwhile is lost. The program counts as ready when it has taken the previous key (cleared the strobe) and has read the keyboard from every place it read it from while the first key waited to be taken (its input loop). If it starts waiting for a different kind of input first (a prompt or question), the rest of the keys are not pressed and the call fails, listing what was pressed, so no key lands in the answer; answer it and send the rest (the answer and the moves after it can go in one call, once an earlier call has seen the game's command loop). No `delay_ms`, joystick entries, `wait_for_read` false or `delay_from` with it; not for a program that never clears the strobe (Conan: use `delay_from` `read`). With `until_ready` true as well, the call returns only once the program is ready for another key after the last one, so `look` or a strip shows the finished move; the report ends with a `then:` line (ready, with its time; a different kind of input, when the last key brought up a prompt, which still succeeds; or not ready within 600 frames). Answering a prompt with it ends ready once the game is back in its command loop; only before any `wait_for_ready` call has seen that loop (straight after a reset) does it wait the full 600 frames. For game manoeuvres, especially with `--realtime`, where the agent's think time would otherwise land between keys. With `look` true it also returns the screen after the last key (a 560x384 PNG), so no second call is needed. With `frames` (2-16) and `every_ms` instead, it returns a strip as `look` does, its first tile taken as soon as the last key has been read and released and its tile times counted from the last key's press, so a whole manoeuvre is captured. `region` and `zoom` apply to `look` or the strip; strip arguments are checked before any key is pressed. |
| `set_joystick` | `x?`, `y?` (0-255), `button0?`, `button1?`, `duration_ms?` | Moves joystick 0. Returns at once. |
| `wait` | `ms` (1-60000) | Lets emulated time pass. |
| `look` | `region?` (`[x, y, w, h]`), `zoom?` (1-8), `frames?` (1-16), `every_ms?` (1-1000) | Returns the screen as a 560x384 PNG image: each Apple pixel (280x192) is drawn 2x2. With `region` (in Apple pixels, x 0-279, y 0-191: halve coordinates read off a screenshot) and/or `zoom` (default 2) it returns just that part, each Apple pixel drawn zoom x zoom (at most 1120x768). At zoom 2 or more every dot of the 560-dot line is shown (Double Hi-Res, 80-column text); zoom 1 shows every other one. With `frames` above 1 (and `every_ms`, required then) it takes that many screenshots `every_ms` of emulated time apart and returns one contact sheet (4 tiles per row, 2 grey pixels between, region and zoom applying to each tile, at most 1280x1024) after a line `tile times: 0.0, 100.1, ... ms`; that much time passes, and a bad argument is rejected before any does. |
| `wait_until_change` | `timeout_ms` (1-60000), `region?` (`[x, y, w, h]`), `look?` (default true), `since?` (`now` or `last_input`, default `now`) | Lets time pass until the screen changes or `timeout_ms` of emulated time is up, then returns `changed after N ms` or `no change in N ms` (a timeout is not an error), plus the whole screen as a 560x384 PNG when `look` is true. With `since` `last_input` it compares with the screen as it was just after the last input tool sent its input, so a change made during that call counts at once; N then counts from that input and the text ends `(since the last input)`. `region` (Apple pixels, 280x192: halve screenshot coordinates) only selects which pixels are watched; every dot of the 560-wide frame inside it counts, so a one-dot Double Hi-Res change is seen. In realtime a change lasting a single frame can be missed. A flickering region (cursor, animation) changes at once. In a realtime session the game keeps running. Long timeouts take real time to run in a stepped session (it steps and fetches a frame at a time). |
| `read_screen` | none | Returns the video mode, then 24 lines of text. Listed only with `--text-screen`. Character mapping: see `get_text_screen` in [docs/api_spec.md](../docs/api_spec.md). |
| `listen` | `ms?` (0-60000, default 0) | Lets `ms` of time pass first (if above 0), then returns the `sound_started` and `sound_stopped` events (with duration and dominant frequency) since the last `listen`. |
| `insert_disk` | `drive` (1 or 2), `path`, `write_protected?` | Puts a floppy image in slot 6 drive 1 or 2, replacing any disk there. A relative `path` is taken from the server's working directory. The harness works on a copy; the original is never written. Paths starting with `\` or `//` (UNC shares, `\?\` and `\.\` device paths) are rejected. |
| `eject_disk` | `drive` (1 or 2) | Takes the floppy out of the drive. An empty drive is an error. |
| `export_disk` | `drive` (1 or 2), `path` | Saves the floppy in the drive, with everything written to it so far, as a new image file in the disk's own format. The disk stays in the drive. |
| `set_speed` | `speed?` (`authentic`, `max` or 0-40), `disk_boost?` | Realtime only (an error in stepped mode). Changes the speed while the machine runs, with the same values as `--speed` and `--no-disk-boost`. Anything left out is unchanged; with no arguments it reports the current speed. |
| `checkpoint` | `name?` (up to 64 characters) | Stepped only (absent with `--realtime`). Saves the whole machine and both floppies (AppleWin's save state plus the working disks) and returns its id (`cp-1`, `cp-2`, ...) and emulated time. Up to 256 per session. |
| `restore_checkpoint` | `checkpoint_id` | Stepped only. Puts the machine and its floppies back as they were at the checkpoint; input still waiting is dropped. The same input after the same checkpoint gives the same result, so a manoeuvre can be retried with different timing. |
| `list_checkpoints` | none | Stepped only. The session's checkpoints, oldest first: id, name (`-` if none), emulated time. |
| `delete_checkpoint` | `checkpoint_id` | Stepped only. Deletes a checkpoint. |

No other operator method (sessions, stepping, state checksums) is reachable through the server, and no checkpoint tool exists in a realtime session.

## Behaviour notes
- **Time.** The default is stepped mode: the emulator is paused, and time passes only through `wait` (and the short stretch `type_text` and `press_key` need, and `listen` with `ms`). A program does not run while the agent is thinking. With `--realtime` the emulator runs on the wall clock, and `wait` sleeps for that many real milliseconds (at a speed above authentic, set by `--speed` or `set_speed`, more emulated time passes).
- **Booting.** Nothing happens until time passes. After launching with a DOS 3.3 disk, call `wait` for about 12000 ms (720 frames) to let the disk boot.
- **Typing.** `type_text` blocks until the Apple has read the keys, plus a few frames for the program to echo them, so a `look` straight afterwards shows the result. If a key isn't read within 600 frames, the tool returns an error and the rest of the text is dropped (the program may be busy or not reading keys). Call `wait`, then look. `press_key` behaves like a real keyboard: it does not wait for the program to read the previous key, it replaces it. It returns once the key is released, plus the same few frames.
- **Sound.** The server subscribes to audio at launch, so `listen` reports sound from launch onwards, not only from the previous `listen`. `listen` with no events reports "no sound events since the last listen".
- **Swapping disks.** `insert_disk` and `eject_disk` do not reboot the machine or pause it. After a swap, DOS needs a command such as `CATALOG` (or `CATALOG,D2`) to see the new disk. Drives are numbered 1 and 2.
- **Saving disks.** `export_disk` writes wherever the agent says, so only give it to an agent you trust with the filesystem. The harness refuses to overwrite a file, to write over a disk's original image, or to write into its own work directory; the adapter also rejects UNC and device paths. The directory must already exist. A relative `path` is taken from the adapter's working directory.
- **Errors.** A harness error comes back as an MCP tool error with the harness message. That includes the harness limits (`-32602` for an out-of-range value such as `press_key` `hold_ms` over 10000, `-32010` when the input queue is full). The adapter's own caps (`wait`, `listen`) are unchanged.
- **Version.** `applewin_mcp.__version__` is read from the `VERSION` file at the repository root, or from `adapter/VERSION` in the release bundle; if neither exists it is `0.0.0+unknown`.

## Tests
```
set HARNESS_EXE=%CD%\bin\x64\Release\AppleWinHarness.exe
python -m unittest discover -s tests/adapter -v
```
Run from the repository root. The tests launch the server headless over stdio with the MCP SDK's client (spec pass conditions 1 and 3).
