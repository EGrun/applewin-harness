# M5 Agent Adapter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** an MCP server that gives an LLM agent the Apple II's keyboard, joystick, screen, sound and the passage of time, plus `get_text_screen` in the harness, and an example task.

**Architecture:**
- The harness gains `get_text_screen`.
- A Python package `adapter/applewin_mcp` launches the harness, owns the operator connection, and exposes agent-only MCP tools over stdio through the official `mcp` SDK.
- **Tests come first (Task 1).**

**Tech Stack:** C++20/MSVC (harness), Python 3.12+ and `mcp==2.3.0` (adapter), `unittest`.

**Spec:** `docs/specs/m5.md` (binding).

## Global Constraints
- **Dependency:** `mcp==2.3.0` only, pinned in `adapter/requirements.txt`. Nothing else new.
- **Tools:** the names are fixed as `type_text`, `press_key`, `set_joystick`, `wait`, `look`, `read_screen` (only with `--text-screen`) and `listen`.
- **Ranges:** `wait.ms` 1–60,000; `listen.ms` 0–60,000. `look` returns a 560×384 PNG.
- **`get_text_screen`:** modes `40col`, `80col`, `mixed40`, `mixed80`, `graphics`; always 24 lines; text lines exactly 40 or 80 characters; `-32006` without `allow_text_screen`.
- **stdout** is reserved for MCP in the adapter.
- **Shared client:** a single `harness_client.py` lives in `adapter/applewin_mcp/`, and the tests import it.
- **Goldens unchanged.** No AppleWin modification.

## Review Focus
1. **Logging corrupts the MCP stream.** Expected: nothing but MCP frames on stdout. The harness child's stdout (the announcement) is read by the adapter, never inherited. *(Task 1 test: run the adapter through the SDK client; any stray stdout breaks the session, and the test asserts a clean tool round trip.)*
2. **The harness process leaks when the agent disconnects.** Expected: closing stdin ends the adapter, which runs `close_session`. The harness's parent watch is a backstop. *(Task 1 test: close the client, then the harness pid exits within 10 s.)*
3. **Typed text never appears because time doesn't pass in stepped mode.** Expected: the tool descriptions say "call `wait`". Also, `type_text` waits internally until its `input_applied` arrives (stepping as needed) before replying, so a `look` straight after shows the text. *(Task 1 test: `type_text`, then immediately `read_screen`, shows the text.)*
4. **Text decoding of inverse and flashing characters, and 80-column interleave.** *(Task 1 C++ `text:` tests with known bytes.)*
5. **`listen` loses events between calls.** Expected: events are buffered by the adapter from launch. *(Task 1 test: bell, `wait`, then `listen` shows one start/stop pair; a second `listen` shows none.)*

---

### Task 1: Interfaces, stubs, and the failing M5 tests
**Files:**
- **Create:**
  - `adapter/requirements.txt`, `adapter/applewin_mcp/__init__.py`, `adapter/applewin_mcp/__main__.py`, `adapter/applewin_mcp/server.py` (stub tools that raise "not implemented");
  - move `tests/python/harness_client.py` to `adapter/applewin_mcp/harness_client.py` and update the imports in `tests/python`;
  - `tests/adapter/test_mcp.py`, `tests/text_screen_test.cpp`, and Python `get_text_screen` tests in `tests/python/test_pipe.py`.
- **Modify:** `src/server/session.cpp` (the `get_text_screen` stub stays `-32006`, but `allow_text_screen: true` is now accepted), `src/host/emulator.h` (`TextScreen ReadTextScreen() const` stub), and the vcxproj.
- [ ] **Step 1: write the tests.**
  - **`text:` (in-process):** pass condition 2 with known bytes and soft switches, covering all five modes, page 2, and the inverse/flash/MouseText mapping.
  - **Python:** `get_text_screen` over the pipe after DOS boot (40col, with the `]` prompt row), and `-32006` without `allow_text_screen`.
  - **Adapter:** pass conditions 1 and 3 and Review Focus 1, 2, 3 and 5.
- [ ] **Step 2:** `pip install -r adapter/requirements.txt`. Build with 0 warnings. Old tests pass; new tests fail. Commit: `test: M5 failing adapter and text-screen tests`.

### Task 2: Harness `get_text_screen`
**Files:** `src/host/emulator.{h,cpp}`, `src/server/session.cpp`.
- [ ] Implement it from AppleWin's soft-switch state (`GetVideo()` mode flags, `MemGetMainPtr`/`MemGetAuxPtr` for the text pages) and the character mapping. Run `-tc="text:*"` and the Python suite. Commit: `feat: get_text_screen`.

### Task 3: MCP server
**Files:** `adapter/applewin_mcp/server.py`, `adapter/applewin_mcp/__main__.py`.
- [ ] **Step 1:** Implement the launch, the tools and shutdown as the spec describes, using `mcp`'s server API (FastMCP or the low-level server, whichever 2.3.0 recommends).
  - `type_text` and `press_key` step or wait until their `input_applied` arrives (Review Focus 3). Cap it at 600 frames, after which the tool returns an error saying the keyboard wasn't read.
  - `look` returns MCP image content.
  - `listen` drains the buffered events.
- [ ] **Step 2:** Run the adapter tests and the full Python suite. Commit: `feat: MCP agent adapter`.

### Task 4: Example task, CI and docs
**Files:**
- **Create:** `adapter/examples/basic_count.md`, `adapter/README.md`.
- **Modify:** `.github/workflows/ci.yml` (pip install and run the adapter tests), `README.md`, `project.md`, `docs/api_spec.md` (`get_text_screen`), `docs/decisions.md` (D4 note: the MCP adapter is the agent surface).
- [ ] Write the example with the Claude Code and Claude Desktop config snippets, using absolute-path placeholders. Validate the YAML. Commit: `docs: M5 example task, adapter docs, CI`.
- [ ] Don't mark M5 ✅. That waits for the human acceptance (pass condition 4).
