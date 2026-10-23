# Security

## Reporting a vulnerability
Please report security problems privately, through GitHub's "Report a vulnerability" button on the repository's Security tab, not in a public issue. I will answer as soon as I can; this is a one-person project, so there is no guaranteed response time.

## What the harness protects, and what it does not
The harness is meant to run on one machine, for one user, with an AI agent that user chooses to run. Its threat model:

- **The pipe is local and owned by the launching user.** `--serve` listens on a named pipe whose access list has one allow entry for the launching user; remote clients are refused. Each role (`agent`, `operator`) needs its own random 256-bit token, which is handed only to the process that launched the harness and is never written to disk.
- **The agent can only do what a person at the machine could** (keys, joystick, screen, sound, disk swaps). It cannot read emulated memory, pause the machine in a realtime session or reach the operator methods. The MCP adapter exposes the agent role only.
- **Disk images are never written in place.** The harness works on copies in its own temp folder.
- **The agent is trusted with the filesystem in two ways.** `insert_disk` reads any disk image path it is given, and `export_disk` writes a new file wherever the agent says (it will not overwrite an existing file, a disk's original image or the harness's own work directory). Do not point the adapter at an agent you would not trust to read and create files as you.
- **Emulated software is untrusted input, not untrusted code.** A disk image runs inside the emulated Apple II, not on your machine, but a bug in the emulator or the harness's parsing of an image could still be exploitable. Do not load images from sources you would not open in any emulator.

A weakness in any of these is in scope. A problem in AppleWin itself belongs [upstream](https://github.com/AppleWin/AppleWin).

## Supported versions
Only the latest release.
