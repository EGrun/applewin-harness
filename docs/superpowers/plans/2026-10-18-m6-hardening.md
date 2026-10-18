# M6 Hardening and Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** input and request limits, structured logging with operator `log` notifications, `disk_written`, the D10 8.3-path mitigation, MIT/GPL licensing (D6), and a portable release zip that CI builds and tests end to end.

**Architecture:**
- **Harness:** gains one logger (`src/host/log.{h,cpp}`) that every diagnostic goes through. Limits are checked in `Session` before anything is queued. `disk_written` is detected from AppleWin's disk-write path, without modifying AppleWin (Review Focus 4).
- **Release:** a PowerShell script assembles the zip from a Release build.
- **Tests come first (Task 1).**

**Tech Stack:** C++20/MSVC v143, doctest, Python 3.12+ (`unittest`, `mcp==2.3.0`), PowerShell 5.1+.

**Spec:** `docs/specs/m6.md` (binding).

## Global Constraints
- **Limits:**
  - `hold_ms` 0–10,000; `duration_ms` 0–60,000; `inter_key_delay_ms` 0–1,000; `text` 4,096 characters (`-32602`).
  - Any string parameter: 32,767 UTF-16 code units (`-32602`).
  - Pending input events: 4,096, all-or-nothing (`-32010`, with `data {pending, limit}`).
  - Request size: 1 MiB (connection closed, one `warning`). Reply and notification frames stay at 16 MiB.
- **Logging:**
  - Flag: `--log-format text|json`, default `text`.
  - JSON line fields: `ts` (UTC ISO-8601 with ms), `level` (`debug`, `info`, `warning` or `error`), `msg`, `component`, plus extras.
  - `log {level, message, emulated_ms}` goes to every operator connection, for `warning` and `error` only.
  - Never log tokens or typed text.
- **`disk_written`:** `{slot: 6, drive}`, sent after `subscribe_disk_writes`. One notification per write burst: a burst ends after 1 emulated second with no write, or at `checkpoint`, `export_disk`, `eject_disk` or `close_session`.
- **D10:** insert working copies through ASCII 8.3 short paths when needed. `working_path` stays the long path.
- **Licensing:**
  - Owned files are MIT, each with an `SPDX-License-Identifier: MIT` header.
  - The binaries are GPL-2.0-or-later.
  - The license files are `LICENSE` (MIT), `LICENSE-GPL-2.0.txt`, `NOTICE.txt` and `THIRD-PARTY-NOTICES.txt`.
  - No disk images in the repository or the bundle.
- **Version:** `VERSION` = `0.1.0`. It is the single source for the zip name, the startup log line, `hello.server_version` (new) and the adapter's `__version__`.
- **Bundle:** `dist/applewin-harness-<version>-win64.zip`, laid out exactly as the spec says. It contains no `.pdb`, `.lib`, tests or disk images.
- **No AppleWin modification. Goldens unchanged.** stdout stays MCP only in the adapter.

## Review Focus
1. **A limit check queues part of a request.** Expected: all or nothing. *(Task 1 test: with the queue at 4,090, a 10-character `type_text` gets `-32010` and the queue count is unchanged.)*
2. **Logging leaks secrets or corrupts protocol streams.**
   - No token or typed text ever appears in a log.
   - In JSON mode, every stderr line parses as JSON.
   - The harness's stdout announcement line is unchanged.

   *(Task 1 tests: grep the logs for the token and the typed text; parse every stderr line.)*
3. **A `log` notification storm.** Expected: a client can't make the server log without bound. Limit rejections are logged at most once per connection per second, with a count of the suppressed lines. *(Task 1 test: 1,000 bad requests give at most a few `log` notifications.)*
4. **`disk_written` needs an AppleWin patch.** Expected: no patch. Detect writes from the harness side, for example by comparing the drive's track-buffer dirty state or the working file's size and mtime after each step, or through an existing AppleWin hook. If none is reliable, stop and report.
5. **The bundle works only on the build machine.** Expected: the extracted bundle runs from any directory, with no repository paths. *(Task 1/8 test: the bundle smoke test runs from `$RUNNER_TEMP` with a cwd outside the repo.)*

---

### Task 1: Failing M6 tests and stubs
**Files:**
- **Create:**
  - `VERSION`;
  - `tests/python/test_limits.py`, `tests/python/test_logging.py`, `tests/python/test_licensing.py`;
  - adapter tests in `tests/adapter/test_mcp.py`, including a bundle smoke test gated on `HARNESS_BUNDLE_DIR`.
- **Stubs:**
  - `--log-format` is accepted and ignored;
  - `subscribe_disk_writes` and `unsubscribe_disk_writes` return `-32603` "not implemented";
  - `hello` returns `server_version` read from `VERSION` at build time. If that is simpler done in Task 2, the test may fail until then.
- [ ] **Step 1: write tests for pass conditions 1–8 and Review Focus 1, 2, 3 and 5.**
  - The D10 test is skipped when 8.3 names are disabled on the volume.
  - The licensing test checks SPDX headers and license files, and the bundle layout only when `HARNESS_BUNDLE_DIR` is set.
- [ ] **Step 2: check and commit.** Build with 0 warnings. Old tests pass and the new ones fail. Commit: `test: M6 failing hardening, logging, licensing tests`.

### Task 2: Logger, `--log-format`, `log` notifications, version
**Files:** `src/host/log.{h,cpp}`, `src/main.cpp`, `src/server/session.cpp`, `src/server/pipe_server.*`, `src/host/emulator.cpp`, `src/host/harness_frame.cpp`, the vcxproj files, and the version plumbing.
- [ ] Route every stderr write through the logger. Add the JSON format and the operator `log` fan-out with rate limiting (Review Focus 3). Add the startup info line with the version, and `hello.server_version`.
- [ ] Run `test_logging.py` and the full suites. Commit: `feat: structured logging and log notifications`.

### Task 3: Input and request limits
**Files:** `src/server/session.cpp`, `src/server/pipe_server.*`, `docs/api_spec.md` (Limits, the new error `-32010`).
- [ ] Implement the value caps, the string cap, the all-or-nothing queue cap and the 1 MiB request cap. Log rejections at `warning`.
- [ ] Run `test_limits.py` and the full suites. Commit: `feat: input and request limits`.

### Task 4: `disk_written`
**Files:** `src/server/session.*`, plus the host layer as needed.
- [ ] Add the subscription and burst detection, with no AppleWin patch (Review Focus 4). Document it in `api_spec.md`.
- [ ] Commit: `feat: disk_written notifications`.

### Task 5: D10 8.3 short paths
**Files:** the disk insert helper that `create_session`, `insert_disk` and restore all use, and `docs/decisions.md` (D10 note).
- [ ] Use `GetShortPathNameW` when the long path isn't ANSI-safe. Relax the `checkpoint` refusal to cases with no ASCII path.
- [ ] Commit: `feat: ASCII short paths for non-ASCII work directories (D10)`.

### Task 6: Adapter logging, version, limit pass-through
**Files:** `adapter/applewin_mcp/{__init__,__main__,server}.py`, `adapter/README.md`.
- [ ] Add `--log-format`, forwarded to the harness. Set `__version__` from `VERSION`; the bundle copies `VERSION` next to the adapter, or the script stamps it in. Pass harness limit errors through as tool errors.
- [ ] Run the adapter tests. Commit: `feat(adapter): structured logging and version`.

### Task 7: Licensing
**Files:** `LICENSE`, `LICENSE-GPL-2.0.txt`, `NOTICE.txt`, `THIRD-PARTY-NOTICES.txt`, SPDX headers on every owned source file, the `README.md` License section, `docs/decisions.md` (D6: Accepted), and `third_party/README.md`.
- [ ] Copy the license texts verbatim from their sources: the AppleWin submodule's `LICENSE`, and zlib, minizip, libyaml and nlohmann/json from their headers or license files in the tree.
- [ ] Run `test_licensing.py`. Commit: `docs: MIT/GPL licensing (D6)`.

### Task 8: Release script, CI, docs
**Files:**
- **Create:** `tools/make_release.ps1`.
- **Modify:** `.github/workflows/ci.yml`, `README.md` (Install from the release zip), `project.md` (the M6 status and the ROM risk) and `docs/api_spec.md` (`server_version`; the protocol stays "0.3" because every M6 change is additive).
- [ ] Assemble the bundle as the spec describes, and rewrite the example paths. In CI's Release job: build the bundle, extract it, run the bundle smoke test and the licensing bundle check, then upload the artifact. Validate the YAML.
- [ ] Commit: `feat: release bundle and CI`.
- [ ] Don't tag or publish a GitHub release. That waits for the user.
