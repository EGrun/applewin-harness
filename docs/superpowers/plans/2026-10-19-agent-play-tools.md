# Agent Play Tools Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the MCP agent cheaper, sharper ways to watch and play a realtime game — screenshots with `press_keys`, crop/zoom, frame strips, `wait_until_change`, joystick entries and per-key `wait_for_read` in `press_keys` — without any power a player lacks.

**Architecture:** Image work lives in a new pure-Python module `adapter/applewin_mcp/screen.py` (Apple-pixel sampling, crop/zoom, contact sheets, change detection, PNG encoding with `zlib`/`struct`), used by new tool options in `adapter/applewin_mcp/server.py`. Joystick entries and `wait_for_read` extend the harness's existing sequence queue entry (`src/host/emulator.cpp`, `QueuedOp::Kind::Sequence`) and `press_keys` parsing (`src/server/session.cpp`).

**Tech Stack:** Python 3.12+ with `mcp==2.3.0` (`mcp.server.mcpserver`: `MCPServer`, `Image`, `ToolError`), C++20 / MSBuild (`AppleWinHarness.sln`, Debug x64), doctest, `unittest`.

**Spec:** `docs/superpowers/specs/2026-10-19-agent-play-tools-design.md`

## Global Constraints

- No checkpoints, no memory reads, no harness pause in realtime for the agent (user ruling, 2026-10-19).
- The adapter's only dependency stays `mcp`; PNG encoding uses `zlib` and `struct`.
- Every time a tool reports or accepts is emulated ms (D10), in both modes.
- Screen tools work on the 560×384 borderless frame; any other `get_frame` size is a tool error "unsupported video mode for region/zoom/strips". A plain `look` with no arguments is unchanged (the harness's own PNG).
- Regions are Apple pixels `[x, y, w, h]` inside 280×192, `w`, `h` ≥ 1; `zoom` 1–8, default 2, nearest-neighbour; output at most 1,120×768.
- Text-and-image results return the text first, then the image (a list `[str, Image]`; the SDK flattens it). `ToolError` is text only.
- Strip: `frames` 1–16, `every_ms` 1–1,000 (required when `frames > 1`), 4 tiles per row, 2-px gaps and empty cells RGB (128, 128, 128).
- `wait_until_change`: `timeout_ms` 1–60,000 emulated ms, realtime wall-clock safety stop at 2 × `timeout_ms`; a timeout is not an error.
- Stage order, one branch each: Stage 1 = Tasks 1–2 (continues on `agent-play-tools-design`, which carries the design and this plan); Stage 2 = Tasks 3–4 and Stage 3 = Tasks 5–6, each on a branch from `origin/main` after the previous stage is merged (not from `origin/HEAD`, which is the stale `m2-windowed-io`).
- Gates, every stage: `bin\x64\Debug\HarnessTests.exe -tse=windowed,audio`; `python -m unittest discover -s tests/python`; `python -m unittest discover -s tests/adapter` (all with `HARNESS_EXE` = the Debug exe). Commit messages end with `Co-Authored-By: Claude Code <noreply@anthropic.com>`.

## Review Focus

1. A region that runs off the screen (`[270, 0, 20, 10]`) or has zero size — expected: a tool error naming the limit, never a crash or a wrapped image. → Task 1 test `test_region_outside_screen_is_rejected`.
2. `look` with `frames > 1` and no `every_ms`, or a strip whose sheet would exceed 1,120×768 — expected: a tool error before any time passes. → Task 3 test `test_strip_parameter_errors`.
3. `wait_until_change` with `timeout_ms: 1` — expected: returns "no change in N ms" within one frame, no hang. → Task 4 test `test_wait_until_change_tiny_timeout`.
4. A realtime `wait_until_change` that ends (change, timeout or error) must leave no frame subscription behind — expected: later tools see no stale `frame_ready` effects and the session's notification backlog doesn't grow. → Task 4 test `test_wait_until_change_unsubscribes`.
5. A `press_keys` of joystick entries only (`[{"joystick": {"x": 0}, "duration_ms": 100}]`) — expected: completes after its restore, report lines `1. joystick  applied 0.0 ms`. → Task 5 C++ test and Task 6 adapter test.

---

## Stage 1 (branch `agent-play-tools-design`)

### Task 1: `screen.py` — Apple pixels, crop/zoom, PNG encoding

**Files:**
- Create: `adapter/applewin_mcp/screen.py`
- Test: `tests/adapter/test_screen.py` (pure unit tests; no harness)

**Interfaces:**
- Produces:
  - `APPLE_W = 280`, `APPLE_H = 192`, `FRAME_W = 560`, `FRAME_H = 384`, `MAX_OUT_W = 1120`, `MAX_OUT_H = 768`
  - `class ScreenError(ValueError)` — message is the user-facing tool error text.
  - `apple_pixels(bgra: bytes, width: int, height: int) -> bytes` — 280×192 RGB (3 bytes per pixel, rows top first), sampling the top-left pixel of each 2×2 block; raises `ScreenError("unsupported video mode for region/zoom/strips")` unless `width, height == 560, 384` and `len(bgra) == 560*384*4`.
  - `check_region(region: list[int] | None) -> tuple[int, int, int, int]` — `None` → `(0, 0, 280, 192)`; otherwise four ints with `w, h ≥ 1`, `x + w ≤ 280`, `y + h ≤ 192`, else `ScreenError("region must be [x, y, w, h] inside 280x192 Apple pixels")`.
  - `crop_zoom(apple_rgb: bytes, region: tuple[int,int,int,int], zoom: int) -> tuple[int, int, bytes]` — `(width, height, rgb)`; `zoom` outside 1–8 → `ScreenError("zoom must be 1 to 8")`.
  - `check_size(width: int, height: int) -> None` — `ScreenError("the image would be WxH; the limit is 1120x768")` when over.
  - `encode_png(width: int, height: int, rgb: bytes) -> bytes` — 8-bit RGB PNG, filter 0, one IDAT.
  - `region_differs(ref_rgb: bytes, cur_rgb: bytes, region: tuple[int,int,int,int]) -> bool` — on 280×192 RGB buffers (used by Task 4).

- [ ] **Step 1: Write the failing tests** in `tests/adapter/test_screen.py`:
  - `test_apple_pixels_samples_top_left_of_each_block`: a 560×384 BGRA buffer where pixel (2i, 2j) is (B=i%256, G=j, R=7) and others are 0 → `apple_pixels(...)[3*(j*280+i):][:3] == bytes([7, j, i % 256])` for `(i, j)` in `[(0,0), (279,191), (100,50)]`.
  - `test_apple_pixels_rejects_other_sizes`: `apple_pixels(bytes(600*420*4), 600, 420)` raises `ScreenError` with "unsupported video mode".
  - `test_region_outside_screen_is_rejected`: `check_region([270, 0, 20, 10])`, `[0, 0, 0, 5]`, `[0, 0, 5]` and `[-1, 0, 5, 5]` raise `ScreenError`; `check_region(None) == (0, 0, 280, 192)`.
  - `test_crop_zoom_nearest_neighbour`: a 280×192 RGB buffer with pixel (10, 20) = (255, 0, 0) → `crop_zoom(buf, (10, 20, 2, 1), 3)` is `(6, 3, rgb)` where the first 3×3 block is red and the rest is the source's pixel (11, 20).
  - `test_size_limit`: `check_size(1120, 768)` passes; `check_size(1121, 768)` raises with "1121x768" and "1120x768".
  - `test_encode_png_round_trip`: decode `encode_png(3, 2, rgb)` with `zlib` (parse chunks, check IHDR 3×2 depth 8 colour type 2, strip the filter byte per row) and get `rgb` back.
  - `test_region_differs`: identical buffers → False; one changed pixel inside the region → True; outside → False.
- [ ] **Step 2: Run** `python -m unittest tests.adapter.test_screen -v` — expected: FAIL, `ModuleNotFoundError: applewin_mcp.screen` (the test file adds `adapter/` to `sys.path` as `test_mcp.py` does).
- [ ] **Step 3: Implement** the functions above in `adapter/applewin_mcp/screen.py` (stdlib only).
- [ ] **Step 4: Run** `python -m unittest tests.adapter.test_screen -v` — expected: all PASS.
- [ ] **Step 5: Commit** `feat(adapter): screen helpers for crop, zoom and PNG encoding`.

### Task 2: `look` region/zoom, `press_keys` `look`, scale in descriptions

**Files:**
- Modify: `adapter/applewin_mcp/server.py` (`Machine.look`, `Machine.press_keys`, the `look` and `press_keys` tools, every tool description that mentions the screen)
- Modify: `adapter/README.md` (tool table), `docs/specs/m5.md` (tool table)
- Test: `tests/adapter/test_mcp.py`

**Interfaces:**
- Consumes: Task 1's `apple_pixels`, `check_region`, `crop_zoom`, `check_size`, `encode_png`, `ScreenError`.
- Produces:
  - `Machine.frame_rgb() -> tuple[bytes, int]` — `(apple_rgb, emulated_cycles)` from one `get_frame { format: "BGRA32", borders: false }` (Tasks 3, 4 use it).
  - `Machine.look(region=None, zoom=None) -> bytes` — PNG; with both `None`, today's harness PNG unchanged.
  - `Machine.press_keys(keys, look=False) -> str | list` — the key report, or `[report, png_bytes]` when `look`.
  - Tools: `look(region: list[int] | None = None, zoom: int | None = None)`; `press_keys(keys, look: bool = False)`.

- [ ] **Step 1: Write the failing tests** in `tests/adapter/test_mcp.py` (stepped, DOS booted with `wait ms=12000`):
  - `test_look_region_and_zoom`: `look(region=[0, 0, 40, 8], zoom=4)` → one PNG of 160×32; `look(zoom=1)` → 280×192; `look()` → 560×384 (unchanged); `look(region=[270, 0, 20, 10])` is a tool error containing "280x192".
  - `test_press_keys_look_returns_report_then_screen`: `press_keys(keys=["P"], look=True)` → content `[text, image]`, text starts `1. P  applied 0.0 ms`, image is 560×384.
  - Extend `test_exactly_the_agent_tools_with_text_screen`'s schema check: the `look` tool's description contains "560x384" and "280x192".
- [ ] **Step 2: Run** `python -m unittest tests.adapter.test_mcp -v` — expected: the new tests FAIL (unexpected keyword / wrong sizes).
- [ ] **Step 3: Implement** in `server.py`: `frame_rgb`, `look(region, zoom)` (crop/zoom path when either is given: `check_region` → `crop_zoom` → `check_size` → `encode_png`; `ScreenError` → `ToolError`), the `look` flag on `press_keys` (screenshot after `await_input`'s settle frames, no extra stepping), and the descriptions: "a 560x384 PNG: each Apple pixel (280x192) is drawn 2x2; regions are in Apple pixels".
- [ ] **Step 4: Run** the adapter suite — expected: PASS (30 + new).
- [ ] **Step 5: Run the gates; commit** `feat(adapter): look region/zoom and press_keys look`; push the branch.

## Stage 2 (branch from `origin/main` after Stage 1 is merged)

### Task 3: Frame strips on `look`

**Files:**
- Modify: `adapter/applewin_mcp/screen.py` (add `contact_sheet`), `adapter/applewin_mcp/server.py` (`Machine.look`, `look` tool), `adapter/README.md`, `docs/specs/m5.md`
- Test: `tests/adapter/test_screen.py`, `tests/adapter/test_mcp.py`

**Interfaces:**
- Consumes: Task 2's `Machine.frame_rgb`; Task 1's helpers.
- Produces: `contact_sheet(tiles: list[tuple[int, int, bytes]], per_row: int = 4, gap: int = 2, gap_rgb: tuple[int,int,int] = (128,128,128)) -> tuple[int, int, bytes]` (all tiles the same size); `Machine.look(region=None, zoom=None, frames=1, every_ms=None) -> bytes | list`; tool `look(region=None, zoom=None, frames: int = 1, every_ms: int | None = None)`, returning `["tile times: 0.0, 100.1, ... ms", Image]` when `frames > 1`.

- [ ] **Step 1: Write the failing tests:**
  - `test_contact_sheet_layout` (unit): five 2×1 tiles → sheet `(4*2 + 3*2, 2*1 + 1*2) == (14, 4)`; tile 5 at row 2 column 1; the gap pixels and the three empty cells are (128, 128, 128).
  - `test_strip_parameter_errors` (adapter, stepped): `look(frames=3)` (no `every_ms`), `look(frames=17, every_ms=10)`, `look(frames=2, every_ms=1001)` and `look(frames=8, every_ms=10, zoom=8)` (sheet over 1120×768) are tool errors; on the JMP-loop screen (static), `look(zoom=1)` before and after the failed calls returns identical PNGs (validation happens before any time passes).
  - `test_strip_tile_times_are_exact_in_stepped_mode`: `look(frames=4, every_ms=100, zoom=1)` → text times within one frame (16.7 ms) of `0, 100, 200, 300`, strictly increasing; image size `(4*280 + 3*2, 192)`.
  - `test_strip_realtime_smoke` (adapter launched with `--realtime`): `look(frames=2, every_ms=200, zoom=1)` → two increasing times, second ≥ 150 ms.
- [ ] **Step 2: Run** — expected: FAIL.
- [ ] **Step 3: Implement** `contact_sheet` and the strip path: validate everything (`check_region`, `zoom`, `frames`, `every_ms`, sheet size via `check_size`) before capturing; stepped: capture, then `advance(ceil(every_ms * 60 / 1000))` between tiles; realtime: capture, `time.sleep(every_ms / 1000)`; times from each capture's `emulated_cycles` via `cycles_to_ms`, relative to the first.
- [ ] **Step 4: Run** — expected: PASS.
- [ ] **Step 5: Commit** `feat(adapter): frame strips on look`.

### Task 3b: Full-resolution sampling (Double Hi-Res)

Added 2026-10-19 at the user's request ("we need to support double high-resolution"). Region/zoom, strips and change detection sample the full 560×384 frame instead of 280×192 Apple pixels, so Double Hi-Res, Double Lo-Res, 80-column text and NTSC colour keep every dot. Tool parameters and output sizes do not change.

**Files:**
- Modify: `adapter/applewin_mcp/screen.py`, `adapter/applewin_mcp/server.py` (`Machine.frame_rgb` and its callers, `look` and `press_keys` descriptions), `adapter/README.md`, `docs/specs/m5.md`, `docs/superpowers/specs/2026-10-19-agent-play-tools-design.md` (§3: replace the top-left 2×2 sampling rule and the 280×192 caveat added in Stage 1; §4 strip cap 1,280×1,024 per the Task 3 ruling; §5: change detection on frame pixels)
- Test: `tests/adapter/test_screen.py`, `tests/adapter/test_mcp.py`

**Interfaces:**
- Produces (replacing Task 1's `apple_pixels` and Task 2's `frame_rgb` contract):
  - `frame_pixels(bgra: bytes, width: int, height: int) -> bytes` — 560×384 RGB (BGRA → RGB, every pixel); same `ScreenError("unsupported video mode for region/zoom/strips")` rule as `apple_pixels` had. `apple_pixels` is removed.
  - `crop_zoom(frame_rgb: bytes, region: tuple[int,int,int,int], zoom: int) -> tuple[int, int, bytes]` — output `(w*zoom, h*zoom)` as before; output pixel `(ox, oy)` takes frame pixel `(2*x + (2*ox)//zoom, 2*y + (2*oy)//zoom)`. Zoom 2 over the whole screen equals the frame; zoom ≥ 2 shows every frame column; zoom 1 shows every other one.
  - `region_differs(ref_rgb: bytes, cur_rgb: bytes, region) -> bool` — on 560×384 frame buffers, comparing every frame pixel inside the region scaled ×2.
  - `Machine.frame_rgb() -> tuple[bytes, int]` — `(frame_pixels(...), emulated_cycles)`.
- Unchanged: `check_region`, `check_size`, `encode_png`, `contact_sheet`, `MAX_OUT_*`, `MAX_SHEET_*`, all tool parameters.

- [ ] **Step 1: Write the failing tests:**
  - `test_frame_pixels_keeps_every_pixel` (unit, replaces `test_apple_pixels_samples_top_left_of_each_block`): a 560×384 BGRA buffer with pixel (x, y) = (B=x%256, G=y%256, R=7) → `frame_pixels` pixel (x, y) is `(7, y%256, x%256)` for `(x, y)` in `[(0,0), (559,383), (1,1), (301,7)]`; the wrong-size rejection test is kept, renamed for `frame_pixels`.
  - `test_crop_zoom_keeps_every_dot_at_zoom_2_and_up` (unit): a frame whose odd columns are white and even columns black → `crop_zoom(f, (0, 0, 4, 1), 2)` is 8×2 with columns alternating black/white; at zoom 3 (12×3) both colours appear in every pair of output columns' source span and no source column in `0..7` is skipped (collect the set of source columns hit = `{0..7}`); at zoom 1 (4×1) only even (black) columns appear.
  - `test_crop_zoom_zoom_2_whole_screen_is_the_frame` (unit): random 560×384 frame → `crop_zoom(f, (0,0,280,192), 2)` returns `(560, 384, f)`.
  - `test_region_differs_sees_a_single_odd_column` (unit): change only frame pixel (2*10+1, 2*5+1) → `region_differs(..., (10, 5, 1, 1))` is True and `(11, 5, 1, 1)` is False.
  - Update Task 1's `test_crop_zoom_nearest_neighbour` for the new source mapping (the red source pixel is frame pixel (20, 40)).
  - `test_look_zoom_2_matches_plain_look_in_80_columns` (adapter, stepped): boot; `type_text("PR#3\n")`, `type_text("HOME:PRINT \"MIXED Il1|\"\n")`, `wait ms=500`; decode both `look()` (the harness PNG) and `look(zoom=2)` to RGB with a small test-local PNG decoder (zlib + PNG filter types 0–4) and assert the pixel data is identical.
- [ ] **Step 2: Run** `python -m unittest tests.adapter.test_screen -v` and the new adapter test — expected: FAIL (`frame_pixels` missing; zoom-2 look differs from plain look).
- [ ] **Step 3: Implement** the interfaces above; Task 3's strip path and the Stage 1 look path keep working through `frame_rgb` and `crop_zoom`. Tool descriptions: replace the Stage 1 caveat with "at zoom 2 or more every dot of the 560-dot line is shown (Double Hi-Res, 80-column text); zoom 1 shows every other one". Keep "560x384" and "280x192" in the `look` description.
- [ ] **Step 4: Run** the full adapter suite — expected: PASS.
- [ ] **Step 5: Commit** `feat(adapter): full-resolution sampling for Double Hi-Res`.

### Task 4: `wait_until_change`

**Files:**
- Modify: `adapter/applewin_mcp/server.py` (`Machine.pump` routes `frame_ready`; new `Machine.wait_until_change`; new tool), `adapter/README.md`, `docs/specs/m5.md`
- Test: `tests/adapter/test_mcp.py` (`ALL_TOOLS` gains `"wait_until_change"`)

**Interfaces:**
- Consumes: Task 3b's `region_differs` (frame pixels, region scaled ×2) and `Machine.frame_rgb` (560×384 RGB); Task 1 `check_region`.
- Produces: tool `wait_until_change(timeout_ms: int, region: list[int] | None = None, look: bool = True)` → `["changed after N ms" | "no change in N ms", Image]` (just the text when `look` is false); `Machine.pump` keeps the latest `frame_ready` params in `self.frame_ready` (a list it appends to only while a wait is subscribed).

- [ ] **Step 1: Write the failing tests** (stepped unless noted):
  - `test_wait_until_change_sees_printed_text`: boot; `type_text("10 FOR I=1 TO 500:NEXT:PRINT \"X\"\n")`, `type_text("RUN\n")`; `wait_until_change(timeout_ms=5000)` → text `changed after N ms` with `N > 0`, image 560×384; `read_screen` contains a line `X`.
  - `test_wait_until_change_times_out_without_error`: JMP loop (`POKE 768,76:POKE 769,0:POKE 770,3:CALL 768`, then `wait ms=500`); `wait_until_change(timeout_ms=300)` → not an error, text `no change in N ms` with `300 ≤ N < 320`.
  - `test_wait_until_change_tiny_timeout`: same loop; `timeout_ms=1` → `no change in N ms`, `N ≤ 17`.
  - `test_wait_until_change_region_only_watches`: on the JMP loop, `region=[0, 0, 280, 8]` → no change; the returned image is still 560×384.
  - `test_wait_until_change_unsubscribes` (`--realtime`): two `wait_until_change(timeout_ms=200)` calls on the JMP loop each return `no change`, and a `listen` afterwards still works and returns promptly (under 2 s wall).
  - Range errors: `timeout_ms=0` and `60001` are tool errors.
- [ ] **Step 2: Run** — expected: FAIL (unknown tool).
- [ ] **Step 3: Implement:** reference = `frame_rgb()`; **stepped:** loop `advance(1)` then `frame_rgb()` and `region_differs`, until changed or emulated elapsed ≥ `timeout_ms`; **realtime:** `subscribe_frames { every_n_frames: 1 }` in a `try`/`finally` that sends `every_n_frames: 0`, wait on `frame_ready` via `pump`, fetch `frame_rgb()` per notification, stop on change, emulated elapsed ≥ `timeout_ms`, or wall time ≥ 2 × `timeout_ms`. Elapsed is from `emulated_cycles` via `cycles_to_ms`.
- [ ] **Step 4: Run** — expected: PASS.
- [ ] **Step 5: Run the gates; commit** `feat(adapter): wait_until_change`; push; the user merges.

## Stage 3 (branch from `origin/main` after Stage 2 is merged)

### Task 5: Harness — joystick entries and `wait_for_read` in `press_keys`

**Files:**
- Modify: `src/host/emulator.h` (`SequenceKey`, `KeyTiming`, `QueueKeys` comment), `src/host/emulator.cpp` (`SeqStep`, the timed branch of `ProcessDueInput`, `QueueKeys`, `PendingInputEvents`, `FinishTimedSequence`), `src/server/session.cpp` (`PressKeys` parsing, `SendInputApplied` item shape), `docs/api_spec.md` (`press_keys`), `docs/decisions.md` (D10 note "Joystick entries and wait_for_read")
- Test: `tests/input_queue_test.cpp`, `tests/python/test_pipe.py`

**Interfaces:**
- Produces (C++):
  - `struct SequenceKey { KeySpec key{uint8_t(0)}; uint32_t delayMs = 0; uint32_t holdMs = 50; bool waitForRead = true; std::optional<JoystickChange> joystick; uint32_t durationMs = 0; };` — a joystick entry has `joystick` set and ignores `key`/`holdMs`/`waitForRead`.
  - `struct KeyTiming { uint64_t appliedCycle = 0, readCycle = 0; bool joystick = false; };`
  - Wire: a joystick item in `input_applied.keys` is `{ "applied_cycle": N, "joystick": true }` (no `read_cycle`); key items unchanged.
- Rules (spec §6–7): `delayMs` counts from the previous entry's applied cycle; a key with `waitForRead` waits for the program's read of the most recent key before it in the sequence (joystick entries in between don't reset that); a joystick entry waits for nothing; `durationMs` restores the state from just before the entry (same mechanism as `QueueJoystick`'s `JoyRestore`, with `completes = false`); completion = last key read (if any) and every hold and restore done (extend `FinishTimedSequence`'s pending scan to `JoyRestore` ops with the sequence id); pending-event count: key 1, joystick 1 or 2 with a duration.
- Session parsing: entry `{joystick: {x?, y?, buttons?}, delay_ms?, duration_ms?}` with `set_joystick`'s ranges (`duration_ms` 0–60,000), `joystick` key `"joystick": 1` inside not allowed — reuse `SetJoystick`'s field parsing (extract a `ParseJoystickChange(const json&) -> JoystickChange` helper from `Session::SetJoystick`); `wait_for_read` must be a boolean and is `-32602` on a joystick entry; an entry with both `key` and `joystick` is `-32602`.

- [ ] **Step 1: Write the failing C++ tests** in `tests/input_queue_test.cpp`:
  - `press_keys: a joystick entry applies after its delay and restores after its duration`: power-cycled (nothing reads keys); `QueueKeys({joy(x=10, delay 20, duration 50)})` → after 1 frame (~17 ms) `e.ScheduledCycle(id)` is still `nullopt`; after 2 more frames it has a value `>= queuedAt + MsToCycles(20)` and `TakeAppliedInputs()` has nothing for `id` (restore pending); after 4 more frames the `InputApplied` has `keys.size() == 1`, `keys[0].joystick`, `lastCycle - firstCycle >= MsToCycles(50)`.
  - `press_keys: a key after a joystick entry waits for the last key's read`: DOS BASIC; `{A, joy(x=0, delay 0), B(delay 0)}` → B's `appliedCycle >= keys[0].readCycle`, and `keys[1].appliedCycle <= keys[2].appliedCycle`; screen `]AB`.
  - `press_keys: wait_for_read false overwrites an unread key`: power-cycled; `{A, B(waitForRead=false)}` → after 1 frame the latch is `0x80|'B'`; the sequence later times out reporting both keys.
  - `press_keys: joystick entries only`: `{joy(x=0, duration 100)}` completes after the restore with one joystick item.
- [ ] **Step 2: Write the failing pipe tests** in `tests/python/test_pipe.py`: `press_keys` with a joystick entry → notification item `{"applied_cycle": int, "joystick": True}` and no `read_cycle`; validation `-32602` for `{"joystick": {"x": 256}}`, `{"key": "A", "joystick": {"x": 1}}`, `{"joystick": {"x": 1}, "wait_for_read": False}`, `{"key": "A", "wait_for_read": 1}`; `-32006` for `{"joystick": {"buttons": {"2": True}}}`.
- [ ] **Step 3: Run** the new C++ tests (`HarnessTests.exe -tc=press_keys:*`) and pipe tests — expected: FAIL / compile error on the new fields.
- [ ] **Step 4: Implement** the struct fields, queue rules, parsing and notification shape; update `docs/api_spec.md` `press_keys` and add the D10 note.
- [ ] **Step 5: Run** `HarnessTests.exe -tc=input:*,press_keys:*` and the pipe suite — expected: PASS.
- [ ] **Step 6: Commit** `feat: joystick entries and wait_for_read in press_keys`.

### Task 6: Adapter — report and describe joystick entries

**Files:**
- Modify: `adapter/applewin_mcp/server.py` (`key_report`, `Machine.press_keys` expected-time sum, the `press_keys` tool description), `adapter/README.md`
- Test: `tests/adapter/test_mcp.py`

**Interfaces:**
- Consumes: Task 5's wire shape.
- Produces: report lines `N. joystick  applied T ms` for joystick items; key lines unchanged; the expected-time sum adds each joystick entry's `delay_ms + duration_ms`.

- [ ] **Step 1: Write the failing test** `test_press_keys_with_joystick_entries` (stepped, DOS booted): `press_keys(keys=[{"joystick": {"x": 0}, "duration_ms": 100}, "A"])` → not an error; lines `1. joystick  applied 0.0 ms` and `2. A  applied … read …`; and `press_keys(keys=[{"joystick": {"x": 0}, "duration_ms": 100}])` alone → one `joystick` line.
- [ ] **Step 2: Run** — expected: FAIL (KeyError `read_cycle` or wrong text).
- [ ] **Step 3: Implement** the report and description changes (description names the joystick entry shape and `wait_for_read`).
- [ ] **Step 4: Run the gates; commit** `feat(adapter): joystick entries and wait_for_read in press_keys`; push; the user merges, then pull `main` and rebuild Release for the tester.
