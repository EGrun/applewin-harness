# Agent play tools — design

Date: 2026-10-19. Status: for the user's approval (builder check done).

## Why

The harness tester plays Conan (an action game) through the MCP adapter in a realtime session. Most of its calls went into
polling with screenshots, stepping through a jump with call pairs, and estimating positions from a full-screen image.
These tools cut the call count and make the screen easier to read, without giving the agent anything a person at the
machine lacks.

## The rule these tools keep

The agent does only what a person at the machine could do (`docs/api_spec.md`, D4, M5 spec). The user ruled on
2026-10-19:
- **No checkpoints and no memory reads for the agent.** They are cheats; they stay operator-only.
- **No pause in realtime** other than the game's own. Stepped mode (the machine frozen between calls) stays available as
  a separate launch mode, for timing practice; nothing here adds pausing to realtime.
- **`wait_until_change` is fair play:** it only watches the screen, as a person waiting for a gate to open would.

## Goals

1. **Tool descriptions say the screen scale:** `look` is 560×384, each Apple pixel (280×192) drawn 2×2.
2. **`press_keys` can return the screen** when the sequence ends (`look: true`), saving the follow-up `look`.
3. **`look` can crop and zoom** a region, so a sprite's position can be read to the Apple pixel.
4. **`look` can return a frame strip:** several frames at an interval in one image, so a jump arc is one call.
5. **`wait_until_change`** returns as soon as a screen region changes, or at a timeout.
6. **`press_keys` can include joystick moves and buttons** in the same timeline as keys.
7. **`press_keys` gets a per-key `wait_for_read`**, so one tool covers both "wait for the program to read the previous
   key" and "overwrite now".

## Non-goals

- Checkpoints, memory reads, a harness-level pause in realtime, or anything else a player cannot do.
- Removing `press_key`: it stays as the one-key shortcut, unchanged, so existing callers keep working.
- Text drawn into images (it would need a font); times go in the tool's text result instead.
- New dependencies. The adapter keeps `mcp` as its only dependency; PNG encoding uses `zlib` and `struct`.

## Anti-goals

- A tool that makes a realtime game wait for the agent.
- Different results between stepped and realtime beyond what timing forces (realtime is best effort on exact frames;
  stepped is exact).
- Images the agent can't read reliably: no smoothing on zoom, no lossy formats.

## Constraints

- Harness API changes follow the existing rules: spec first (`docs/api_spec.md`, a D10 note), tests first, parameters
  validated by the harness with `-32602`, input counted against the 4,096 pending-event cap.
- Adapter tools are described for an LLM, return short text and/or a PNG, and turn harness errors into tool errors.
- Stepped results stay deterministic.
- The work is done in this order, each on its own branch for the user to merge: (1) goals 1–3, adapter only;
  (2) goals 4–5, adapter only; (3) goals 6–7, harness and adapter. Each branch starts from `origin/main` after the
  previous one is merged (`origin/HEAD` still points at the old `m2-windowed-io` branch; don't branch from it).
- Gates for every branch: `bin\x64\Debug\HarnessTests.exe -tse=windowed,audio`, `python -m unittest discover -s
  tests/python`, `python -m unittest discover -s tests/adapter` (with `HARNESS_EXE` set), on this Windows machine
  (MSBuild, `AppleWinHarness.sln`, Debug x64).

## Decisions made while writing this (builder check answers)

- **Frame size:** the screen tools work on the 560×384 borderless frame only (the //e configuration has no VidHD
  card). A `get_frame` of any other size is a tool error ("unsupported video mode for region/zoom/strips"); a plain
  `look` with no arguments still returns whatever the harness gives.
- **Time:** every time these tools report or accept is emulated ms (D10), measured from `emulated_cycles` /
  `frame_ready` cycles, in both modes. In realtime, `wait_until_change`'s `timeout_ms` is also emulated time; the
  adapter adds a wall-clock safety stop at twice `timeout_ms` (scaled by nothing else). At authentic speed the two agree.
- **Watching frames:** the adapter already drains notifications on its agent connection (`Machine.pump` routes
  `input_applied` and sound events), so realtime `wait_until_change` subscribes to `frame_ready` and fetches a frame per
  notification (BGRA32, about 860 KB). The builder may fetch PNG and decode instead if BGRA can't keep up; the
  requirement is detecting a change within one presented frame at authentic speed.
- **Results with images:** a tool with text and an image returns the text first, then the image (`look` in strip mode,
  `press_keys` with `look`, `wait_until_change`). Changing `look`'s return type to a list for those modes is fine: the
  clients are LLMs.
- **Encoder output:** any `look` with arguments (even `zoom: 2` over the whole screen) uses the adapter's encoder;
  only the no-argument `look` is the harness's PNG. Filter and compression settings are the builder's.
- **Strip layout:** gap and empty cells (a short last row) are RGB (128, 128, 128).
- **`press_keys` + `look`:** no extra frames: the screenshot is taken after the settle frames the tool already steps,
  so step counts in existing tests don't change.

## Design

### 1. Screen scale in descriptions
`look`, and every tool that returns a screenshot, says: 560×384 PNG, each Apple pixel 2×2; coordinates in regions are
Apple pixels, 0–279 across and 0–191 down.

### 2. `press_keys { …, look?: bool = false }` (adapter)
With `look: true` the result is the per-key text followed by the screen as a PNG, taken after the settle frames the tool
already waits for. A dropped sequence is still a tool error; it includes the screen when `look` is true. *(builder's
call: whether the MCP SDK lets a tool error carry an image; if not, the error is text only.)*

### 3. `look { region?: [x, y, w, h], zoom?: int }` (adapter)
- `region` is in Apple pixels (inside 280×192, `w`, `h` ≥ 1); default the whole screen.
- `zoom` is the size of one Apple pixel in the output, 1–8; default 2 (today's scale). Nearest-neighbour only.
- The output must stay at most 1,120×768 pixels (the whole screen at zoom 4); a larger request is a tool error saying
  the limit.
- The adapter fetches `get_frame { format: "BGRA32" }` (560×384, top row first), samples one source pixel per Apple pixel
  (the top-left of each 2×2 block), and encodes an RGB PNG itself. The cropped or zoomed image is the 280x192 Apple-pixel picture (the top-left pixel of each 2x2 block), so it loses the extra horizontal detail of 80-column text, double-hires and NTSC colour; a plain look shows the full 560x384 frame.
- With no arguments the result is byte-for-byte today's `look` (the harness's own PNG). *(my call: keeps the existing
  M5 behaviour and its tests unchanged.)*

### 4. `look { frames?: int = 1, every_ms?: int, region?, zoom? }` (adapter)
- `frames` 1–16; with `frames > 1`, `every_ms` is required, 1–1,000.
- One image: the tiles left to right, wrapping after 4 per row, a 2-pixel grey gap between tiles. `region` and `zoom`
  apply to each tile; a strip sheet may be at most 1,280×1,024 (so four full-screen tiles at zoom 1, 1,126 px wide, fit), while a single-image `look` keeps 1,120×768.
- The text result lists each tile's emulated time in ms, relative to the first tile.
- **Stepped:** the first tile is now; then the adapter steps `every_ms` (rounded up to whole frames) and captures, so
  tile times are exact.
- **Realtime:** the adapter captures the latest frame, sleeps `every_ms` of wall time, and repeats; each tile is labelled
  with the time the harness reports for it (`get_frame`'s `emulated_cycles`). Best effort: the spacing is the wall
  clock's.

### 5. `wait_until_change { region?: [x, y, w, h], timeout_ms: int, look?: bool = true }` (adapter)
- Takes the current frame as the reference, then checks every presented frame until any pixel in `region` (Apple
  pixels, default the whole screen) differs from the reference, or `timeout_ms` (1–60,000) passes.
- Returns text: `changed after N ms` or `no change in N ms` (a timeout is not an error), plus the screen as a PNG when
  `look` is true (the default). `region` crops nothing in the returned image: it only selects what is watched.
  *(my call: the agent needs to see the whole scene to decide what to do next.)*
- **Stepped:** steps one frame at a time; time passes only for the wait, exactly as `wait` does.
- **Realtime:** the game keeps running; the adapter watches frames as they are presented (a `subscribe_frames`
  subscription or polling `get_frame` each frame: builder's call, whichever detects a change within one frame) and
  returns at the first changed frame.
- A region that flickers (a blinking cursor, an animation) changes at once; the agent picks a region that excludes it.

### 6. Joystick entries in `press_keys` (harness and adapter)
- A `keys` entry may be `{ joystick: { x?, y?, buttons?: { "0"?, "1"? } }, delay_ms?: int = 0, duration_ms?: int = 0 }`,
  with the same ranges and joystick-0-only rule as `set_joystick` (`joystick: 1` and button `"2"` are `-32006`).
- It is applied `delay_ms` after the previous entry (of either kind) was applied (the first: after the call), and never
  waits for a keyboard read; `wait_for_read` is not allowed on it (`-32602`). With `duration_ms` the joystick returns to
  the state it had just before this entry was applied, after that long, exactly as a queued `set_joystick` restore
  does today (overlapping restores apply in due order, as now).
- `delay_ms` always counts from the previous entry's applied cycle, whatever its kind. A key entry waits, in addition,
  for the program to have read the most recent key before it in the sequence (if there is one and `wait_for_read` is
  true); joystick entries in between don't reset that.
- The sequence completes when the last key has been read (if it has keys) and every hold and joystick restore has
  finished. A sequence of joystick entries only completes when its last restore finishes.
- `input_applied.keys` gains one item per entry, in request order. A key item is `{ applied_cycle, read_cycle }` as
  now; a joystick item is `{ applied_cycle, joystick: true }` with no `read_cycle`, so a never-read key
  (`read_cycle: null`) can't be confused with it. *(my call: one list, indexed like the request.)* The field keeps its
  name; `docs/api_spec.md` and the adapter's `key_report` are updated (joystick lines read `joystick  applied N ms`).
- The 256-entry limit counts every entry. Each joystick entry counts against the pending-event cap as `set_joystick`
  does (1, or 2 with a duration).

### 7. `wait_for_read` per key in `press_keys` (harness and adapter)
- A key entry may set `wait_for_read: false`: that key does not wait for the program to have read the previous key
  (it overwrites the latch, as `press_key` does); its `delay_ms` still counts from the previous entry's applied cycle.
  Default true. The first key never waits (unchanged).
- The program's read of every key is still recorded and reported, and the sequence still completes when the last key
  has been read (600-frame timeout as now).

## Testing

- Adapter, goals 1–5: unit tests for the PNG encoder (decode with `zlib` and compare pixels), region and zoom maths, the
  strip layout, and parameter errors; end-to-end tests in stepped mode on DOS 3.3 (exact tile times; `wait_until_change`
  returns "changed" when typed text appears in its region and "no change" at the timeout); one realtime smoke test per
  tool.
- Harness, goals 6–7: C++ input-queue tests (joystick entry timing and restore, a key after a joystick entry, overwrite
  with `wait_for_read: false`, timings list order) and pipe tests for validation and the notification shape.
- `look` with no arguments: the existing M5 tests keep passing unchanged.

## Left to the builder

- How realtime `wait_until_change` watches frames (subscription or polling), provided it detects a change within one
  presented frame.
- Whether a tool error can carry an image (goal 2).
- Internal structure of the PNG encoder and image helpers.
