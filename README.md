# AppleWin Harness

A harness that lets an AI agent use an emulated Apple II through AppleWin. The agent can only do what a person could: type on the keyboard, move the joystick, look at the screen, and listen. Experiments are safe (disks run on working copies) and reproducible (pause, step, checkpoint).

**Status:** M0 is done (scaffolding). Next is the M1 spike, which hosts the AppleWin core headless. See [project.md](project.md).

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
AppleWinHarness.exe --boot-frames <n> [--png <path>]
```
Boots an Apple //e Enhanced headless (no window, no sound), runs `n` frames as fast as possible, optionally saves the 560x384 screen to `<path>` as a PNG, and prints `sha256 <hash>` of the frame. Exit codes: 0 success, 2 bad arguments (usage on stderr), 1 any other failure (error on stderr).

Display modes become available in M2:

```
AppleWinHarness.exe              # windowed (default): view-only window plus the Apple's sound
AppleWinHarness.exe --headless   # no window or sound, for CI and batch runs
```

## License
Not decided yet. The harness links AppleWin (GPL-2.0), so its license must be GPL-2.0-compatible (decision D6).
