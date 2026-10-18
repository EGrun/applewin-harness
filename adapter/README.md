# applewin_mcp: an MCP server for the Apple II harness

`applewin_mcp` lets an LLM agent use an emulated Apple //e through AppleWin Harness. It is an MCP server over stdio. It offers only what a person at the machine could do: type, press keys, move the joystick, look at the screen, listen, let time pass, and swap floppy disks. Other operator concerns (the disk loaded at launch, the run mode, export, checkpoints) are not tools; they are set at launch (decision D4 in [docs/decisions.md](../docs/decisions.md)). The spec is [docs/specs/m5.md](../docs/specs/m5.md).

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
python -m applewin_mcp --exe <path\to\AppleWinHarness.exe> [--headless] [--disk <image>] [--realtime] [--text-screen]
```

| Flag | Meaning |
|---|---|
| `--exe <path>` | Required. The harness executable. |
| `--headless` | No window and no audio device. Without it the harness shows a view-only window and plays the sound. |
| `--disk <image>` | A disk image for slot 6, drive 1. The harness works on a copy; the original is never written to. Without it the machine starts with no disk. |
| `--realtime` | Run in realtime mode. The default is stepped mode. |
| `--text-screen` | Turn on the `read_screen` tool (the session is created with `allow_text_screen`). |

The server starts the harness (`--serve`), creates the session and serves MCP on stdio. stdout carries MCP frames only; log lines go to stderr. When the server exits it closes the session and the harness exits. To run a task, see [examples/basic_count.md](examples/basic_count.md).

## Tools
| Tool | Parameters | What it does |
|---|---|---|
| `type_text` | `text`, `inter_key_delay_ms?` | Types on the keyboard (`\n` is RETURN; printable ASCII, up to 4096 characters). |
| `press_key` | `key`, `hold_ms?` | Presses one key: a character, an ASCII code, or a name such as RETURN, ESC, TAB, DELETE, LEFT, RIGHT, UP, DOWN, CTRL-A to CTRL-Z, or RESET. |
| `set_joystick` | `x?`, `y?` (0-255), `button0?`, `button1?`, `duration_ms?` | Moves joystick 0. Returns at once. |
| `wait` | `ms` (1-60000) | Lets emulated time pass. |
| `look` | none | Returns the screen as a 560x384 PNG image. |
| `read_screen` | none | Returns the video mode, then 24 lines of text. Listed only with `--text-screen`. Character mapping: see `get_text_screen` in [docs/api_spec.md](../docs/api_spec.md). |
| `listen` | `ms?` (0-60000, default 0) | Lets `ms` of time pass first (if above 0), then returns the `sound_started` and `sound_stopped` events (with duration and dominant frequency) since the last `listen`. |
| `insert_disk` | `drive` (1 or 2), `path`, `write_protected?` | Puts a floppy image in slot 6 drive 1 or 2, replacing any disk there. A relative `path` is taken from the server's working directory. The harness works on a copy; the original is never written. |
| `eject_disk` | `drive` (1 or 2) | Takes the floppy out of the drive. An empty drive is an error. |

No other operator method (sessions, checkpoints, export, speed, stepping) is reachable through the server.

## Behaviour notes
- **Time.** The default is stepped mode: the emulator is paused, and time passes only through `wait` (and the short stretch `type_text` and `press_key` need, and `listen` with `ms`). A program does not run while the agent is thinking. With `--realtime` the emulator runs on the wall clock, and `wait` sleeps.
- **Booting.** Nothing happens until time passes. After launching with a DOS 3.3 disk, call `wait` for about 12000 ms (720 frames) to let the disk boot.
- **Typing.** `type_text` and `press_key` block until the Apple has read the keys, plus a few frames for the program to echo them, so a `look` straight afterwards shows the result. If the keyboard isn't read within 600 frames beyond the time the typing itself takes, the tool returns an error (the program may be busy or not reading keys). Call `wait`, then look.
- **Sound.** The server subscribes to audio at launch, so `listen` reports sound from launch onwards, not only from the previous `listen`. `listen` with no events reports "no sound events since the last listen".
- **Swapping disks.** `insert_disk` and `eject_disk` do not reboot the machine or pause it. After a swap, DOS needs a command such as `CATALOG` (or `CATALOG,D2`) to see the new disk. Drives are numbered 1 and 2.
- **Errors.** A harness error comes back as an MCP tool error with the harness message.

## Tests
```
set HARNESS_EXE=%CD%\bin\x64\Release\AppleWinHarness.exe
python -m unittest discover -s tests/adapter -v
```
Run from the repository root. The tests launch the server headless over stdio with the MCP SDK's client (spec pass conditions 1 and 3).
