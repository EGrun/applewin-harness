# Example task: count to ten in BASIC

## The task
Give the agent this text:

> Write and run an Applesoft BASIC program that prints the numbers 1 to 10, one per line.

The agent has the Apple //e through the `applewin` MCP server, with a DOS 3.3 disk in drive 1 of slot 6. It must boot the disk (time only passes through `wait`), type the program, and `RUN` it.

## Success check
After `RUN`, the screen shows the numbers 1 to 10, one per line (with `read_screen`, lines containing `1` to `10`, then the `]` prompt). The person checking can see it with `look`, or in the harness window if the server was started without `--headless`.

A program that would do it:

```
10 FOR I = 1 TO 10
20 PRINT I
30 NEXT I
RUN
```

## Configuration
Replace the placeholders with absolute paths on your machine:
- `<REPO>`: the repository root.
- `<PYTHON>`: your Python 3.12+ interpreter, with `pip install -r <REPO>/adapter/requirements.txt` done.

The server needs the adapter directory as its working directory (or on `PYTHONPATH`). The disk is `<REPO>\external\AppleWin\bin\DOS 3.3 System Master - 680-0210-A.dsk`. The harness works on a copy, so the original isn't changed. Leave out `--headless` to watch the run in the harness window. Add `--realtime` only if you want the Apple to run on the wall clock.

### Claude Code
Put this in `.mcp.json` in your project (or add it with `claude mcp add`):

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

### Claude Desktop
Put this in `claude_desktop_config.json` (Settings, Developer, Edit Config), then restart Claude Desktop:

```json
{
  "mcpServers": {
    "applewin": {
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

## What to expect
1. The agent calls `wait` (about 12000 ms) so the disk boots, then `look` or `read_screen` to see the `]` prompt.
2. It types the program with `type_text` (`\n` is RETURN), then types `RUN`.
3. It calls `wait` briefly, then `read_screen` or `look` and sees 1 to 10.

## Swapping disks
The agent can also change floppies while the machine runs, with `insert_disk` (`drive` 1 or 2, `path`, optional `write_protected`) and `eject_disk` (`drive`). The machine is not rebooted, so after a swap the agent types `CATALOG` (or `CATALOG,D2` for drive 2) and waits to see the new disk. The harness works on a copy; the original image is never written.
