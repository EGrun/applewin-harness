# AppleWin Harness

A harness that lets an AI agent use an emulated Apple II through AppleWin. The agent can only do what a person could: type on the keyboard, move the joystick, look at the screen, and listen. Experiments are safe (disks run on working copies) and reproducible (pause, step, checkpoint).

**Status:** M0 and M1 are done. M2 (window, sound, keyboard, disks) is implemented and awaiting human checks. See [project.md](project.md).

## Documents
- [project.md](project.md) — goal, architecture, milestones, risks
- [docs/api_spec.md](docs/api_spec.md) — the JSON-RPC API
- [docs/decisions.md](docs/decisions.md) — design decisions and their status

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

Examples:
```
:: windowed (default): live view-only window and sound until you close it
AppleWinHarness.exe

:: headless golden run: boot DOS 3.3 (720 frames), type PRINT 2+2, save the screen
AppleWinHarness.exe --headless --disk1 "external\AppleWin\bin\DOS 3.3 System Master - 680-0210-A.dsk" --frames 720 --type "PRINT 2+2\r" --frames 60 --png out.png

:: windowed, real time: the bell
AppleWinHarness.exe --realtime --disk1 "external\AppleWin\bin\DOS 3.3 System Master - 680-0210-A.dsk" --frames 720 --type "PRINT CHR$(7)\r" --frames 120
```

## Tests
```
REM everything
bin\x64\Debug\HarnessTests.exe
REM what CI runs
bin\x64\Debug\HarnessTests.exe -tse=windowed,audio
```
Suites with special needs are tagged: `realtime` (paced runs), `windowed` (needs a desktop session) and `audio` (needs an audio device; includes a 60-second underrun test, about 70 s). CI (`.github/workflows/ci.yml`, windows-2022) builds Debug and Release x64 and runs `HarnessTests.exe -tse=windowed,audio`.

## License
Not decided yet. The harness links AppleWin (GPL-2.0), so its license must be GPL-2.0-compatible (decision D6).
