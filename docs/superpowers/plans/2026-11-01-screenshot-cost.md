# Screenshot Cost Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut what screenshots cost the MCP agent: the adapter fetches frames as PNG instead of raw BGRA, never resends an image the agent already has, and its tool descriptions state the cost of an image.

**Architecture:** All changes are in the Python adapter (`adapter/applewin_mcp`). `screen.py` gains a PNG decoder for the harness's own PNG form. `Machine` gets every frame through one PNG fetch and every outgoing image through one gate, `_offer`, which swaps an image identical to the last one sent for a short note. A module-level `tool_content` turns a `Machine` result into MCP content. The harness and its API do not change.

**Tech Stack:** Python 3.11+, standard library only in `screen.py` (`zlib`, `struct`, `hashlib`), `mcp` 2.3.0, `unittest`.

**Spec:** `docs/decisions.md` D15 (this plan's source). The agent client in `docs/specs/client.md` is a separate piece of work and not part of this plan.

## Global Constraints

- `screen.py` stays standard-library only.
- Every new file starts with `# SPDX-License-Identifier: MIT` (D6).
- No harness change: `get_frame` and its `BGRA32` format stay as they are (D15).
- Error text for a frame of the wrong size on a crop path stays `unsupported video mode for region/zoom/strips`.
- The note text is exactly: `the screen is the same as the last image you were sent ({ms} ms of emulated time ago), so no image is attached; call look with force true to get it again`.
- Only pixels decide "the same": same width, height and RGB bytes (D15, D4).
- Work on branch `screenshot-cost` from `main`. Don't merge or push without asking.
- Commit messages end with `Co-Authored-By: Claude Code <noreply@anthropic.com>`.
- Run the adapter tests with `HARNESS_EXE` set: `python -m unittest discover -s tests/adapter -v`.

## Review Focus

1. **A plain `look` at a size other than 560×384** (a VidHD mode). Today the PNG passes through untouched; after Task 1 it must still return an image, not the crop-path size error. Test in Task 1.
2. **An image recorded as sent but never returned.** If `_offer` runs and the call then fails, the next `look` answers with a note for an image the agent never got. Rule: `_offer` is the last step before a successful return. Test in Task 2 (a failing `press_keys` leaves `last_image` unchanged).
3. **The agent no longer holding the last image** (its client cleared it). `force: true` must always return the image and record it as sent. Test in Task 2.
4. **A repeated `look` in a stepped session with no time passing.** This is the common case and must give the note, end to end through MCP. Test in Task 2.
5. **Existing tests that call `look` twice and expect two images.** They break by design. Fix each one with `force=True`, never by weakening the gate. Step in Task 2.

## Decisions taken for the user (ledger)

- The PNG fetch is in this plan, although D15's token decisions don't need it: one decoded RGB path makes the hash uniform. It is gated on measurement (Task 1, Step 6).
- Only `look` takes `force`. `press_keys` and `wait_until_change` don't, because the agent can call `look` after them.
- One slot: the gate compares with the last image sent, not with every image ever sent, because a client may have cleared older ones.

---

### Task 1: Fetch frames as PNG and decode them

**Files:**
- Modify: `adapter/applewin_mcp/screen.py`. Add `decode_png`; remove `frame_pixels`.
- Modify: `adapter/applewin_mcp/server.py`. `Machine.frame_rgb` and `_look_locked` (fast path).
- Test: `tests/adapter/test_screen.py`, `tests/adapter/test_mcp.py` (`MachineUnitTest`).

**Interfaces:**
- Produces: `screen.decode_png(data: bytes) -> tuple[int, int, bytes]`, giving `(width, height, rgb)` with 8-bit RGB top-down rows. It raises `ScreenError("unexpected PNG from the harness")` for anything that is not 8-bit RGB (colour type 2), uninterlaced, with filter byte 0 on every row and a valid signature. It accepts any number of IDAT chunks.
- Produces: `Machine.frame(self) -> tuple[int, int, bytes, bytes, int]`, giving `(width, height, rgb, png, emulated_cycles)` from one `get_frame` with `format="PNG", borders=False`, at any size.
- Keeps: `Machine.frame_rgb(self) -> tuple[bytes, int]`, giving `(rgb, emulated_cycles)` and raising `ToolError("unsupported video mode for region/zoom/strips")` unless the frame is 560×384.

- [ ] **Step 1: Write the failing tests** in `test_screen.py` (replacing the two `frame_pixels` tests):
  - `test_decode_png_round_trips_encode_png`: `screen.decode_png(screen.encode_png(3, 2, rgb)) == (3, 2, rgb)` for `rgb = bytes(range(18))`.
  - `test_decode_png_accepts_split_idat`: split the IDAT data of `encode_png(4, 4, ...)` into two chunks (with `screen._chunk`); it decodes to the same `(4, 4, rgb)`.
  - `test_decode_png_rejects_other_forms`: a bad signature, colour type 6, interlace 1, and a row with filter byte 1 each raise `ScreenError` with the message `unexpected PNG from the harness`.
- [ ] **Step 2: Write the failing tests** in `MachineUnitTest`, with `m.rpc` stubbed to record its params and return `{"data": base64(png), "width", "height", "emulated_cycles": 1234, "format": "PNG"}`:
  - `test_frame_rgb_fetches_png`: the recorded params are `{"format": "PNG", "borders": False}`, and `m.frame_rgb() == (rgb, 1234)` for a 560×384 frame.
  - `test_frame_rgb_rejects_other_sizes`: a 600×420 frame raises `ToolError` with `unsupported video mode for region/zoom/strips`.
  - `test_plain_look_accepts_other_sizes`: `m._look_locked()` on a 600×420 frame returns PNG bytes that decode to 600×420 (Review Focus 1).
- [ ] **Step 3: Run** `python -m unittest tests.adapter.test_screen tests.adapter.test_mcp.MachineUnitTest -v`. Expected: the new tests FAIL (`decode_png` and `frame` are not defined).
- [ ] **Step 4: Implement** `decode_png` in `screen.py`. Parse the chunks, concatenate IDAT, `zlib.decompress`, check and strip one filter byte per row. Then implement `Machine.frame` and rewrite `frame_rgb` on top of it. The plain-`look` path in `_look_locked` uses `frame()` and returns its `png` unchanged (Task 2 routes it through `_offer`). Remove `frame_pixels` and its import.
- [ ] **Step 5: Run** the same command. Expected: PASS. Then run the whole adapter suite. Expected: PASS.
- [ ] **Step 6: Measurement gate.** With `HARNESS_EXE` set, write a throwaway script in the session scratchpad (not committed). It starts a stepped session on the DOS 3.3 System Master, steps 300 frames, then times 100 `get_frame` round trips with `format="PNG"` and 100 with `format="BGRA32"`, including base64 decoding (plus `decode_png` for PNG), and prints both medians and the reply sizes. Record the numbers in this plan's ledger. If the PNG median is higher than the BGRA32 median, revert Steps 4–5's `frame`/`frame_rgb` change to the BGRA32 fetch (keep `decode_png` for the plain-`look` path), note it in the ledger, and continue with Task 2.
- [ ] **Step 7: Commit**
  ```bash
  git add adapter/applewin_mcp/screen.py adapter/applewin_mcp/server.py tests/adapter/test_screen.py tests/adapter/test_mcp.py
  git commit -m "perf: the adapter fetches frames as PNG and decodes them (D15)"
  ```

### Task 2: Don't resend an image the agent already has

**Files:**
- Modify: `adapter/applewin_mcp/server.py`. `Machine.__init__`, `_offer` (new), `_look_locked`, `look`, `_strip_locked`, `wait_until_change`, the `press_keys` image path, `tool_content` (new, module level), and the `look`, `press_keys` and `wait_until_change` tool wrappers.
- Test: `tests/adapter/test_mcp.py` (`MachineUnitTest`, `EndToEndTest`, and any test fixed by Step 6).

**Interfaces:**
- Consumes: `Machine.frame()` and `frame_rgb()` (Task 1).
- Produces: `UNCHANGED_NOTE` (module constant, the exact text in Global Constraints, with a `{ms}` field).
- Produces: `Machine.last_image: tuple[bytes, int] | None`, holding `(sha256 digest of b"%dx%d:" % (w, h) + rgb, emulated_cycles when sent)`. Set to `None` in `__init__` (and in the `MachineUnitTest.machine()` stub).
- Produces: `Machine._offer(self, width: int, height: int, rgb: bytes, cycles: int, png: bytes | None = None, force: bool = False) -> bytes | str`. It returns PNG bytes (`png`, or `encode_png(width, height, rgb)` when `None`) and records them as sent, unless the image is identical to `last_image` and `force` is false. In that case it returns `UNCHANGED_NOTE.format(ms=int(round(cycles_to_ms(cycles - sent_cycles))))` and records nothing. Call it with `self.lock` held, as the last step before a successful return (Review Focus 2).
- Produces: `Machine.look(self, region=None, zoom=None, frames=1, every_ms=None, force=False)`.
- Produces: `tool_content(result)`. It maps `bytes` to `Image(data=..., format="png")`, `str` to `str`, `[text, bytes]` to `[text, Image]`, and `[text, str]` to `text + "\n" + str`.

- [ ] **Step 1: Write the failing unit tests** in `MachineUnitTest`, with `m.frame` and `m.frame_rgb` stubbed from a settable `(rgb, cycles)`:
  - `test_a_repeated_look_returns_the_note`: the first `m.look()` returns `bytes`; the second, same frame, returns a `str` equal to `UNCHANGED_NOTE.format(ms=0)`.
  - `test_the_note_counts_emulated_time`: the second frame has the same pixels at `cycles + 102180`, and the note says `(100 ms of emulated time ago)`.
  - `test_force_returns_the_image_again`: after a `look`, `m.look(force=True)` returns `bytes` and `last_image` holds the new cycles (Review Focus 3).
  - `test_a_changed_screen_is_sent`: one byte of `rgb` differs, so the result is `bytes`.
  - `test_a_crop_after_a_full_look_is_sent`: `m.look()` then `m.look(region=[0, 0, 280, 192], zoom=1)` both return `bytes`.
  - `test_a_failed_call_records_nothing`: stub `m.rpc` to return `{"input_id": 1}` and `m.await_input` to raise `ToolError("dropped")`; `m.press_keys(["A"], look=True)` raises `ToolError` and `last_image` is unchanged (Review Focus 2).
  - `test_tool_content_maps_each_shape`: the four mappings above.
- [ ] **Step 2: Write the failing end-to-end test** in `EndToEndTest`: in a stepped session after boot, call the `look` tool twice with no time passing between them. The first result's content is one image; the second is one text item starting `the screen is the same as the last image you were sent (` (Review Focus 4). Then `look` with `force: true` returns an image.
- [ ] **Step 3: Run** `python -m unittest tests.adapter.test_mcp.MachineUnitTest tests.adapter.test_mcp.EndToEndTest -v`. Expected: the new tests FAIL.
- [ ] **Step 4: Implement** `UNCHANGED_NOTE`, `last_image`, `_offer`, the `force` parameter and `tool_content`. Route every image through `_offer`:
  - plain `look`: `frame()` with its `png`;
  - crop: the `crop_zoom` output;
  - strips: the contact sheet, with the last tile's cycles;
  - `wait_until_change`: the final 560×384 `cur` and its cycles;
  - `press_keys`: through `_look_locked` and `_strip_locked`.

  The three tool wrappers return `tool_content(result)`, and the `look` tool gains `force: bool = False`.
- [ ] **Step 5: Run** the same command. Expected: PASS.
- [ ] **Step 6: Run the whole adapter suite.** Any test that now fails because a repeated `look` returns the note gets `force=True` on its later `look` (Review Focus 5); no other change to it. Expected: PASS.
- [ ] **Step 7: Commit**
  ```bash
  git add adapter/applewin_mcp/server.py tests/adapter/test_mcp.py
  git commit -m "feat: an image the agent already has is replaced by a short note (D15)"
  ```

### Task 3: Tool descriptions and docs state the cost

**Files:**
- Modify: `adapter/applewin_mcp/server.py` (the docstrings of `look`, `press_keys` and `wait_until_change`).
- Modify: `adapter/README.md` (the tool lines for `look` and `wait_until_change`) and `CHANGELOG.md` (Unreleased).
- Test: `tests/adapter/test_mcp.py` (`ToolListTest`).

**Interfaces:**
- Consumes: the `force` parameter (Task 2).

- [ ] **Step 1: Write the failing test** `test_image_tools_state_their_cost` in `ToolListTest`:
  - the `look` tool's description contains `about 280 tokens` and `force true`;
  - its input schema has a boolean `force` property;
  - `wait_until_change`'s description contains `look false when you only need to know whether something changed`;
  - `press_keys`'s description contains `the same as the last image you were sent`.
- [ ] **Step 2: Run** `python -m unittest tests.adapter.test_mcp.ToolListTest -v`. Expected: FAIL.
- [ ] **Step 3: Edit the docstrings** with this exact copy:
  - **`look`, appended:** "Each image stays in your context: the whole screen costs about 280 tokens, zoom 1 about 70, and a small region 10-40, so use a region when you know where to look and zoom 1 for a rough look. If the image would be the same as the last one you were sent, you get a short note instead; give force true to get the image anyway."
  - **`wait_until_change`**: the sentence "With look true (the default) it also returns the whole screen at that moment as a 560x384 PNG (never cropped to region)." becomes "With look true (the default) it also returns the whole screen at that moment as a 560x384 PNG (never cropped to region), or a note if it is the same as the last image you were sent; give look false when you only need to know whether something changed (an image costs about 280 tokens of your context)."
  - **`press_keys`**: after "without a second call." insert " (or a note if it is the same as the last image you were sent)".
- [ ] **Step 4: Update the docs.** In `adapter/README.md`, the `look` and `wait_until_change` lines mention the note and `force`. In `CHANGELOG.md` under Unreleased, add: "Adapter: frames are fetched as PNG; an image identical to the last one sent is replaced by a note (`look` takes `force`); tool descriptions state image cost (D15)."
- [ ] **Step 5: Run** `python -m unittest discover -s tests/adapter -v`. Expected: PASS.
- [ ] **Step 6: Commit**
  ```bash
  git add adapter/applewin_mcp/server.py adapter/README.md CHANGELOG.md tests/adapter/test_mcp.py
  git commit -m "docs: adapter tools state what an image costs (D15)"
  ```
