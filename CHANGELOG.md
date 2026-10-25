# Changelog

All notable changes to this project are listed here. The version is in [VERSION](VERSION).

## 0.1.0 - unreleased

First public release.

- The harness: `AppleWinHarness.exe` hosts the AppleWin core with a view-only window and sound, or headless. A JSON-RPC server (`--serve`) on a named pipe offers keys, joystick, frames, audio, speed controls, disk swaps and export, checkpoints and state checksums.
- The MCP adapter `applewin_mcp`: lets an LLM agent type, press timed key sequences (`press_keys`, with `delay_from` for read-anchored timing and `wait_for_ready` to send each key only when the game is waiting for a command, stopping at a prompt), use the joystick, look at the screen (regions, zoom, frame strips), wait for changes, listen, swap disks and, in stepped sessions, save and restore checkpoints.
- Stepped and realtime modes; Double Hi-Res and 80-column text are shown at full resolution.
- A release zip (`tools/make_release.ps1`) and a source archive that includes AppleWin (`tools/make_source_archive.ps1`).
- Licensing: MIT for the harness's own code, GPL-2.0-or-later for the binaries that link AppleWin. See [NOTICE.txt](NOTICE.txt).
