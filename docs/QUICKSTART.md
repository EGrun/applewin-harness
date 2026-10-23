# Quick start

From nothing to an LLM agent typing on an emulated Apple //e, in about ten minutes. Windows 10/11 x64 only.

## 1. Get the harness
You need Python 3.12 or later either way.

**From a release zip** (no build): download `applewin-harness-<version>-win64.zip` from the [Releases page](https://github.com/EGrun/applewin-harness/releases), unzip it, and follow "Install from the release zip" in the [README](../README.md#install-from-the-release-zip). Then skip to step 3.

**From source** (needs Visual Studio 2022 with the "Desktop development with C++" workload). In a Developer Command Prompt:

```
git clone --recurse-submodules https://github.com/EGrun/applewin-harness.git
cd applewin-harness
msbuild AppleWinHarness.sln -m -p:Configuration=Release -p:Platform=x64
pip install -r adapter\requirements.txt
```

The exe is `bin\x64\Release\AppleWinHarness.exe`. The DOS 3.3 disk used below comes with the AppleWin submodule, at `external\AppleWin\bin\DOS 3.3 System Master - 680-0210-A.dsk`.

## 2. Check the harness on its own
Boot DOS 3.3, type `PRINT 2+2`, and save the screen:

```
bin\x64\Release\AppleWinHarness.exe --headless --disk1 "external\AppleWin\bin\DOS 3.3 System Master - 680-0210-A.dsk" --frames 720 --type "PRINT 2+2\r" --frames 60 --png out.png
```

It prints `sha256 <hash>` and exits with 0. `out.png` shows the `]` prompt and `4`. Leave out `--headless` (and add `--realtime`) to watch it in a window, with sound.

## 3. Connect an MCP client
The adapter `applewin_mcp` is an MCP server over stdio. In the examples below, `<REPO>` is the repository root (or, for the zip, the folder you unzipped, with the exe directly in it) and `<PYTHON>` is your Python. In JSON, double every backslash.

**Claude Code:** put this in `.mcp.json` in the project you run Claude Code from, then run `/mcp` to check that `applewin` is connected:

```json
{
  "mcpServers": {
    "applewin": {
      "type": "stdio",
      "command": "<PYTHON>",
      "args": [
        "-m", "applewin_mcp",
        "--exe", "<REPO>\\bin\\x64\\Release\\AppleWinHarness.exe",
        "--disk", "<REPO>\\external\\AppleWin\\bin\\DOS 3.3 System Master - 680-0210-A.dsk",
        "--text-screen"
      ],
      "env": { "PYTHONPATH": "<REPO>\\adapter" }
    }
  }
}
```

**Claude Desktop and other clients:** see [adapter/examples/basic_count.md](../adapter/examples/basic_count.md). Any MCP client that runs a stdio server with these arguments works.

A window opens showing the Apple screen. It is view-only: the agent types, you watch. Add `--headless` to the arguments to run without it.

## 4. Give the agent a task
Ask it:

> Write and run an Applesoft BASIC program that prints the numbers 1 to 10, one per line.

It should `wait` about 12000 ms for the disk to boot, `look` or `read_screen` to find the `]` prompt, `type_text` the program and `RUN`, then look again and see 1 to 10. The full task and its success check are in [adapter/examples/basic_count.md](../adapter/examples/basic_count.md).

## 5. Choose a mode
- **Stepped (the default).** The Apple is paused while the agent thinks; time passes only when the agent calls `wait` (or an input tool). Runs are reproducible, and the agent can `checkpoint` and `restore_checkpoint` to retry. Best for puzzles, text adventures, programming and testing.
- **Realtime (`--realtime`).** The Apple runs on the wall clock and never waits for the agent, as for a person. Use it for action games. Send timed manoeuvres in one `press_keys` call, with `delay_from: "read"` for timings tuned to the game's own polling, and `--speed` or `set_speed` to change the speed. Checkpoints are not available.

The agent can never read memory or pause a realtime game: it sees and does only what a player could (see "Fair play" in the [README](../README.md#fair-play)).

## Troubleshooting
| Symptom | Cause and fix |
|---|---|
| The screen stays black or on the AppleWin logo | Nothing runs until time passes. The agent must `wait` (about 12000 ms to boot DOS 3.3). |
| There is no `read_screen` tool | Add `--text-screen` to the server arguments. |
| `type_text` fails with an error after 600 frames | The program is not reading the keyboard (busy, or waiting on something else). `wait`, `look`, then try again. |
| The build fails with `LNK1104` on `AppleWinHarness.exe` | An MCP client still runs the harness. Disconnect it (Claude Code: `/mcp`), then rebuild. |
| The game runs too fast in realtime with a disk in | AppleWin runs at full speed while a drive spins. Add `--no-disk-boost`. |
| The server does not start | Run the `--exe` path with `--version` to check it, and check that `PYTHONPATH` names the `adapter` folder. The server logs to stderr; add `--log-format json` for structured lines. |

## Next steps
- Every tool and its options: [adapter/README.md](../adapter/README.md).
- Driving the harness without MCP (JSON-RPC over a named pipe, Python client): [README](../README.md#serve-json-rpc-over-a-named-pipe) and [docs/api_spec.md](api_spec.md).
- Why it works the way it does: [docs/decisions.md](decisions.md).
