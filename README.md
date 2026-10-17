# AppleWin Harness

A harness that lets an AI agent use an emulated Apple II through AppleWin. The agent can only do what a person could: type on the keyboard, move the joystick, look at the screen, and listen. Experiments are safe (disks run on working copies) and reproducible (pause, step, checkpoint).

**Status:** M0, M1 and M2 (window, sound, keyboard, disks) are done. M3 is done: the `--serve` pipe server, agent audio (`get_audio`, `subscribe_audio`) and speed controls (`set_speed`, `get_speed`). M4 is done: checkpoints (`checkpoint`, `restore_checkpoint`), `get_state_checksum`, `step_cycles` and disk export (`list_disks`, `export_disk`). M5 is implemented and awaiting human acceptance: an MCP agent adapter (`adapter/`) and `get_text_screen`. See [project.md](project.md).

## Documents
- [project.md](project.md) — goal, architecture, milestones, risks
- [docs/api_spec.md](docs/api_spec.md) — the JSON-RPC API
- [docs/decisions.md](docs/decisions.md) — design decisions and their status
- [adapter/README.md](adapter/README.md) — the MCP server for LLM agents

## Prerequisites
- Windows 10/11 x64
- Visual Studio 2022 with the "Desktop development with C++" workload (v143 toolset)
## Getting the source
[AppleWin](https://github.com/AppleWin/AppleWin) is included as a pinned git submodule at `external/AppleWin` (decision D7). Clone with:

```
git clone --recurse-submodules <repo-url>
```

If you already cloned without it, run:

```
git submodule update --init
```

## Build
Open `AppleWinHarness.sln` in Visual Studio, or build from a Developer Command Prompt:

```
msbuild AppleWinHarness.sln -p:Configuration=Debug -p:Platform=x64
```

The executable is written to `bin\x64\<Configuration>\AppleWinHarness.exe`.

## Run
```
AppleWinHarness.exe [--headless] [--realtime] [--disk1 <image>] [--png <path>] [step ...]
step: --frames <n> | --type <text>
```
- Steps run in command-line order. `--frames n` runs `n` frames. `--type text` types the text on the emulated keyboard. Both run at authentic speed, paced to real time, with `--realtime`, and as fast as possible otherwise.
- `--type` escapes: `\r` and `\n` both mean Return, and `\\` means a backslash. Any other backslash sequence, an empty text, or a character outside `0x20`-`0x7E` is a usage error.
- `--frames` takes 1-9 decimal digits.
- `--disk1` boots from a working copy of the image in `%TEMP%\applewin-harness\<pid>\`. The original is never written to, and the copy is deleted on exit.
- After the last step the harness writes the PNG if `--png` was given, prints `sha256 <hash>` of the frame, and, if sound is active, prints `audio underruns <n>`.
- With no steps, windowed mode runs until you close the window and exits with 0. Headless mode with no steps is a usage error.
- `--headless`, `--realtime`, `--disk1` and `--png` may each appear once. Closing the window during a run prints `error: window closed` and exits with 1.
- Exit codes: 0 success, 2 bad arguments (usage on stderr), 1 any other failure.

The default display mode is windowed: a view-only window shows the live Apple screen and plays its sound (keys and mouse clicks in the window do nothing). Sound plays only during realtime runs (`--realtime`, or windowed with no steps); stepped runs without `--realtime` are muted. `--headless` opens no window and no audio device, for CI and batch runs (decision D8).

Speed: `set_speed` and `get_speed` follow AppleWin's own controls (decision D11). With "Enhance disk access speed" on (the default), an empty Disk II drive counts as a running motor, so a realtime or windowed run with no disk spins at full speed; turn `enhance_disk_speed` off for authentic pacing. The window plays sound only at authentic speed, not at full speed; agent audio capture is unaffected (decision D14).

Agent audio: through `--serve`, an agent can hear the Apple with `get_audio` and `subscribe_audio` in every mode, headless and stepped included (decisions D12, D14). In windowed mode the sound you hear lags the window by about 200 to 300 ms.

Examples:
```
:: windowed (default): live view-only window and sound until you close it
AppleWinHarness.exe

:: headless golden run: boot DOS 3.3 (720 frames), type PRINT 2+2, save the screen
AppleWinHarness.exe --headless --disk1 "external\AppleWin\bin\DOS 3.3 System Master - 680-0210-A.dsk" --frames 720 --type "PRINT 2+2\r" --frames 60 --png out.png

:: windowed, real time: the bell
AppleWinHarness.exe --realtime --disk1 "external\AppleWin\bin\DOS 3.3 System Master - 680-0210-A.dsk" --frames 720 --type "PRINT CHR$(7)\r" --frames 120
```

## Serve (JSON-RPC over a named pipe)
```
AppleWinHarness.exe --serve [--headless]
```
- Prints one JSON line on stdout, `{"pipe":"\\\\.\\pipe\\applewin-harness-<id>","agent_token":"...","operator_token":"...","pid":<n>}`, and then serves. `--serve` can't be combined with `--realtime`, `--disk1`, `--png` or any step.
- One session per process. The process exits 0 after `close_session`, or when the window is closed.
- The protocol is in [docs/api_spec.md](docs/api_spec.md).

A Python client using only the standard library lives in `tests/python/harness_client.py`:

```python
import sys
sys.path.insert(0, "tests/python")
from harness_client import Harness

h = Harness.launch(headless=True)
op = h.connect("operator")
op.call("create_session", disks=[{"slot": 6, "drive": 1, "path": r"C:\disks\dos33.dsk"}], run_mode="stepped")
op.call("step_frames", frames=720)
agent = h.connect("agent")
agent.call("type_text", text="PRINT 2+2\n", inter_key_delay_ms=0)
op.call("step_frames", frames=60)
frame = agent.call("get_frame", format="PNG")   # frame["data"] is base64
op.call("close_session")
h.close()
```

Checkpoints (decision D5): `checkpoint` saves the machine and the working disks together, and `restore_checkpoint` brings both back. Restoring the same checkpoint and running the same script gives the same state checksum, frames and audio.

Continuing a stepped session that has booted (as above, before `close_session`):

```python
cp = op.call("checkpoint", name="booted")["checkpoint_id"]      # "cp-1"

def run_script():
    op.call("restore_checkpoint", checkpoint_id=cp)
    agent.call("type_text", text="CATALOG\n", inter_key_delay_ms=0)
    op.call("step_cycles", cycles=500000)
    op.call("step_frames", frames=60)
    return op.call("get_state_checksum")["sha256"]

assert run_script() == run_script()
op.call("export_disk", slot=6, drive=1, dest_path=r"C:\out\dos33-result.dsk")   # keep the working disk
```

Run the Python tests (build first; `HARNESS_EXE` defaults to `bin\x64\Debug\AppleWinHarness.exe`):
```
set HARNESS_EXE=%CD%\bin\x64\Release\AppleWinHarness.exe
python -m unittest discover -s tests/python -v
```

## MCP agent adapter
`adapter/applewin_mcp` is an MCP server (stdio) that lets an LLM agent use the emulated Apple II. See [adapter/README.md](adapter/README.md) and the example task [adapter/examples/basic_count.md](adapter/examples/basic_count.md).

```
pip install -r adapter/requirements.txt
python -m unittest discover -s tests/adapter -v
```
Run the adapter tests from the repository root, with `HARNESS_EXE` set as for the Python pipe tests.

## Tests
```
REM everything
bin\x64\Debug\HarnessTests.exe
REM what CI runs
bin\x64\Debug\HarnessTests.exe -tse=windowed,audio
```
Suites with special needs are tagged: `realtime` (paced runs), `windowed` (needs a desktop session) and `audio` (needs an audio device; includes a 60-second underrun test, about 70 s). CI (`.github/workflows/ci.yml`, windows-2022) builds Debug and Release x64, runs `HarnessTests.exe -tse=windowed,audio`, then the Python pipe tests, and then the adapter tests.

## License
Not decided yet. The harness links AppleWin (GPL-2.0), so its license must be GPL-2.0-compatible (decision D6).
