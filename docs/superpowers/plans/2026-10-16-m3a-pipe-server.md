# M3 Part A — Pipe Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `AppleWinHarness.exe --serve` exposes the emulator over an ACL'd named pipe with JSON-RPC: a session, keyboard, joystick, frames and pause/resume/step. A Python client reproduces the M2 golden over the pipe.

**Architecture:**
- Connection threads (overlapped pipe I/O) parse and authorise requests, then post commands to a queue.
- The main thread owns the emulator, the window and sound. It drains the queue between steps.
- Input events go through a deterministic queue inside `Emulator`, applied at step boundaries.
- **The tests come first (Task 2).**

**Tech Stack:** C++20, MSVC v143, nlohmann/json 3.11.3 (vendored), Win32 named pipes and BCrypt, Python 3 standard library (`unittest`) for the client tests.

**Spec:** `docs/specs/m3.md`, Part A plus its shared sections. It is binding, and `docs/api_spec.md` yields to it.

## Global Constraints
- x64 Debug and Release, v143. Harness code is C++20 at warning level 4 with 0 warnings. No file under `external/AppleWin` may be modified.
- New dependency: `third_party/nlohmann/json.hpp` v3.11.3 only. The Python tests use the standard library only.
- Protocol version is `"0.3"`. The pipe name is `\\.\pipe\applewin-harness-<32 hex>`. Tokens are 64 hex characters. Messages have a 4-byte little-endian length prefix, and the maximum message size is 16 MiB (16,777,216 bytes). At most 8 pipe instances.
- Error codes are those in `api_spec.md`. `type_text` text is limited to 4,096 characters. The default `inter_key_delay_ms` is 30 and the default `hold_ms` is 50. `step_frames` takes 1–100,000 frames. `every_n_frames` takes 0–600.
- The M2 golden `tests/golden/m2_dos_print.sha256` must keep passing, both in-process and over the pipe.
- Doctest suites: `realtime`, `windowed`, `audio` (CI excludes `windowed` and `audio`). Python tests run in CI in headless mode only. Windowed Python tests are skipped unless `HARNESS_WINDOWED=1`.
- Never leave server processes running: tests kill their child processes on every failure path.

## Review Focus
1. **A blocking read stalls replies on the same pipe handle.** Expected: a notification (`input_applied`) arrives while the client is idle in a read. *(Task 2 Python test: send `type_text`, then read the notification without sending anything else.)*
2. **A client disconnects mid-request or mid-message.** Expected: the server keeps serving the other connection, and the session continues. *(Task 2 test: open an agent connection, send half a frame, close it, then the operator calls `step_frames`.)*
3. **A key typed while an earlier key is unread is lost or reordered.** Expected: first in, first out, strobe-gated. *(Task 2 in-process test: `type_text("AB")` while the strobe is set; the screen later shows `AB`.)*
4. **A command arrives during a long `step_frames`.** Expected: it is processed after the step, in order. *(Task 2 Python test: `get_frame` sent on the agent connection during `step_frames 600`; its `frame_id` is ≥ the frame count after the step.)*
5. **Leaked processes or temp files when the client dies.** Expected: the server exits when told to, and a test harness kill leaves only the files the M1 tests already allow. *(Task 2: every Python test uses `addCleanup(harness.close)`. `close()` sends `close_session`, waits 5 s, then kills the process.)*

**Build:** as in M2. **Tests:** `bin/x64/Debug/HarnessTests.exe -tse=windowed,audio`, and `python -m unittest discover -s tests/python -v`.

---

### Task 1: Dependencies — nlohmann/json and base64
**Files:** Create `third_party/nlohmann/json.hpp` (from https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp), `src/core/base64.{h,cpp}`, `tests/base64_test.cpp`. Modify the vcxproj files.
**Interfaces:** Produces `std::string harness::Base64Encode(std::span<const uint8_t>)`.
- [ ] Test the RFC 4648 §10 vectors ("", "f", "fo", "foo", "foob", "fooba", "foobar") plus one 3-MB round-trip length check. Implement, build, run `-tc="base64:*"`. Commit: `build: vendor nlohmann/json 3.11.3; base64 encoder`.

### Task 2: Interfaces, stubs, and the full failing Part A test suite
The implementer writes the tests from the spec only.

**Files:**
- **Create:**
  - `src/server/pipe_server.{h,cpp}`, `src/server/session.{h,cpp}`, `src/server/protocol.{h,cpp}` (headers complete, bodies stubbed);
  - `tests/server_test.cpp`, `tests/input_queue_test.cpp`;
  - `tests/python/harness_client.py`, `tests/python/test_pipe.py`.
- **Modify:**
  - `src/host/emulator.h` and `.cpp`: input-queue declarations, with stubs;
  - `src/main.cpp`: only the `--serve` argument parsing, behind a stub that exits 1;
  - the vcxproj files.

**Interfaces (Produces):**
- **`protocol.h`:**
  - `struct RpcError { int code; std::string message; };`
  - `nlohmann::json MakeResult(const nlohmann::json& id, nlohmann::json result)`
  - `nlohmann::json MakeError(const nlohmann::json& id, int code, std::string message)`
  - `nlohmann::json MakeNotification(std::string method, nlohmann::json params)`
  - `std::optional<uint32_t> ParseLengthPrefix(std::span<const uint8_t,4>)`, which rejects 0 and anything over 16 MiB.
  - Error-code constants named as in `api_spec.md` (e.g. `kUnauthorized = -32001`).
- **`pipe_server.h`:** `class PipeServer`.
  - `PipeServer(std::wstring name, Tokens, std::function<void(ConnectionPtr, nlohmann::json request)> onRequest)` and `Start()`/`Stop()`.
  - `struct Connection` has `Role role()`, `void Send(const nlohmann::json&)` (thread-safe) and `uint64_t id()`.
  - `PipeServer` handles framing, the handshake and role checks before `onRequest`. It is told which methods are operator-only through `std::function<bool(std::string_view)> isOperatorOnly`.
  - `static std::wstring RandomPipeName()` and `static std::string RandomToken()`.
- **`session.h`:** `class Session`.
  - `Session(DisplayMode, Emulator&)`.
  - `void Post(ConnectionPtr, nlohmann::json request)`, which is thread-safe.
  - `int RunMainLoop()`, which returns the process exit code.
  - All method handlers are private.
- **`emulator.h` additions:**
  - `uint64_t QueueText(std::string_view text, uint32_t interKeyDelayMs)`
  - `uint64_t QueueKey(KeySpec, uint32_t holdMs)`
  - `uint64_t QueueJoystick(JoystickChange, uint32_t durationMs)`
  - `std::optional<uint64_t> ScheduledCycle(uint64_t inputId) const`
  - `std::vector<InputApplied> TakeAppliedInputs()`
  - `struct InputApplied { uint64_t inputId, firstCycle, lastCycle; }`
  - `KeySpec` is a variant: ASCII byte, arrow, or reset.
  - `JoystickChange` has optional x, y, button0 and button1.
  - `TypeText` keeps its M2 signature and behaviour.

- [ ] **Step 1: write the tests.**
  - **`input:`** (in-process):
    - `QueueText("PRINT 2+2\r", 0)` gives the same result as `TypeText`: DOS boot, then 60 frames, then the M2 golden.
    - FIFO with the strobe set: `QueueText("AB", 0)` while a key is unread gives `AB` on screen, in order.
    - `inter_key_delay_ms` 30: consecutive `InputApplied` first cycles are at least 30 ms × base clock apart.
    - `press_key` with `hold_ms` 100: `$C010` bit 7 is set during the hold and clear after it (read through `MemGetMainPtr`… or the I/O read function; the implementer picks the honest way to read the AKD flag, for example `KeybReadFlag()`).
    - `RESET` key: the PC lands on the reset vector path. Assert that `]` reappears after `CTRL-RESET` at the BASIC prompt.
    - Joystick: `PDL(0)` reads via a typed `PRINT PDL(0)` at x=0 and x=255 (pass condition 4). Button 0 via `PRINT PEEK(-16287)`.
    - Invalid text throws `std::invalid_argument`.
  - **`server:`** (C++, launching `--serve --headless`):
    - The announcement is a single JSON line with the exact keys `pipe`, `agent_token`, `operator_token` and `pid`, and its format matches the spec's regex.
    - DACL: open the pipe, call `GetSecurityInfo(…, DACL_SECURITY_INFORMATION)`; there is exactly 1 ACE, an allow ACE, and its SID equals the current user's.
    - A length prefix of 0, or of 16 MiB + 1, closes the connection.
    - Malformed JSON gets `-32700` and the connection stays open.
    - Bad token gets `-32001` and the connection closes.
    - A non-`hello` first message gets `-32001`.
    - Agent calling `step_frames` gets `-32001` and the connection stays open.
    - Version `"0.2"` gets `-32600`.
    - An unknown method gets `-32601`.
    - `--serve --disk1 x` exits 2.
  - **Python `test_pipe.py`:**
    - Pass condition 1 (the M2 golden over the pipe).
    - Pass condition 5 (pause/resume).
    - A second `create_session` gets `-32008`.
    - Agent before a session gets `-32002`.
    - `input_applied` arrives unprompted (Review Focus 1).
    - The half-frame disconnect (Review Focus 2).
    - A `get_frame` during a long step (Review Focus 4).
    - A missing disk gets `-32005`; a relative path gets `-32602`.
    - `allow_text_screen: true` gets `-32006`.
    - `subscribe_frames { every_n_frames: 2 }` over `step_frames 10` gives 5 `frame_ready` notifications.
    - `get_frame` BGRA32: `stride == width*4`, and the data length equals `stride*height`.
  - **Windowed** (doctest suite `windowed`): a `--serve` server with no `--headless`. After `create_session`, `WM_CLOSE` to the window exits 0 and the work dir is removed.
- [ ] **Step 2:** Build. Expected: 0 warnings, all M1/M2 tests pass, and the new tests fail. Run Python; expected: failures, no hangs (each test has a 60 s timeout).
- [ ] **Step 3:** Commit: `test: M3A failing test suite and interface stubs`.

### Task 3: Input queue in Emulator
**Files:** `src/host/emulator.{h,cpp}`.
- [ ] Implement the queue as the spec describes, in the step primitive. Re-express `TypeText` as `QueueText` plus running until its `InputApplied` arrives. Switch joystick 0 to `J0C_MOUSE`; per the spec's ruling, stop and report if any M2 golden changes. Map `KeySpec` arrows to `KeybQueueKeypress(VK_LEFT…, NOT_ASCII)`.
- [ ] Run `-tc="input:*,dos:*,emulator:*"` and the full in-process suite. Expected: they pass, and the M2 goldens are unchanged. Commit: `feat(host): deterministic input queue (keys, holds, reset, joystick)`.

### Task 4: Pipe transport
**Files:** `src/server/pipe_server.cpp`, `src/server/protocol.cpp`.
- [ ] Implement it: the security descriptor via `InitializeSecurityDescriptor` plus `SetEntriesInAclW` with one `EXPLICIT_ACCESS` for the token user SID; overlapped `ConnectNamedPipe`; a connection thread with an overlapped read loop; and `Send` doing an overlapped write under a mutex. `Stop()` cancels I/O (`CancelIoEx`) and joins the threads. `RandomToken` uses `BCryptGenRandom`.
- [ ] Run `-tc="server:*"`. Expected: the transport tests pass, including the DACL, framing and handshake tests; the method tests stay red until Task 5. Commit: `feat(server): ACL'd overlapped named-pipe transport with hello handshake`.

### Task 5: Session loop and methods
**Files:** `src/server/session.cpp`, `src/main.cpp`.
- [ ] Implement the states, command dispatch, the methods and notifications as the spec describes. `main.cpp`'s `--serve` path generates the name and tokens, prints the announcement, starts the `PipeServer` with `onRequest = session.Post`, runs `RunMainLoop`, and on exit stops the server and cleans up.
- [ ] Run `-tc="server:*"`, then `-ts=windowed -tc="server:*"`, then `python -m unittest discover -s tests/python -v`. Expected: all pass. Then the full suite in Debug and Release. Commit: `feat(server): session loop, agent and operator methods, --serve`.

### Task 6: CI and docs
**Files:** `.github/workflows/ci.yml`, `docs/api_spec.md`, `project.md`, `README.md`, `docs/decisions.md` (D3: protocol 0.3 and input replies).
- [ ] **CI:** add `actions/setup-python@v5` (3.12) and a step that runs the Python tests with `HARNESS_EXE=bin\x64\<config>\AppleWinHarness.exe`.
- [ ] **`api_spec.md`:** bring it to 0.3 with Part A's changes:
  - the announcement line;
  - `input_id`, nullable `applied_at_cycle` and `input_applied`;
  - top-down BGRA32;
  - `pause`/`step_frames` reply shapes;
  - one session per process;
  - `-32006` for the deferred features;
  - no auto-repeat;
  - `protocol_version`.
- [ ] **`project.md`:** M3 now includes `create_session`/`close_session` (taken from M4); M4 keeps checkpoints and the state checksum.
- [ ] **README:** add a `--serve` section.
- [ ] Validate the YAML. Commit: `ci: Python pipe tests; docs: API 0.3`.
