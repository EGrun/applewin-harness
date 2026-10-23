---
name: Bug report
about: Something in the harness or the MCP adapter does not work as documented
labels: bug
---

**What happened**
What you did, what you expected, and what happened instead. Include the exact error text.

**How to reproduce**
The command line or MCP server arguments, the tool calls (or a short script), and the disk image if it is freely available. Please do not attach copyrighted disk or ROM images.

**Environment**
- Harness version (`AppleWinHarness.exe --version`):
- Release zip or built from source (commit):
- Windows version:
- Python version, and MCP client (for adapter problems):
- Mode: stepped or `--realtime`; windowed or `--headless`

**Logs**
The harness and adapter log to stderr. `--log-format json` gives one JSON object per line.
