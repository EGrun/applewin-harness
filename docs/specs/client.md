# Agent client spec

**Status:** Proposed (D15). For review before implementation.

## 1. Purpose

`applewin_client` is a command-line program that runs one Claude agent against the AppleWin MCP adapter (`adapter/applewin_mcp`) until the agent finishes a goal or a limit is reached. It exists so that the agent loop is ours, which the off-the-shelf MCP clients (Claude Desktop, Claude Code) do not allow:

- **Old screenshots stop costing tokens.** The client asks the Claude API to clear old tool results with context editing (D15). Every image otherwise stays in context for the whole run.
- **Runs are measured.** Every turn's token usage, cache use and clearing is logged, so the effect of D15 can be checked with numbers.
- **Runs are repeatable.** The goal, model, effort and adapter flags are all on the command line, and the transcript records them.

### Non-goals
- No tools beyond the adapter's. The client adds none, and does not filter them (the adapter decides what the agent may do, D4).
- No chat UI, no streaming display, no multi-agent orchestration, no resuming an interrupted run.
- No other model providers.
- No harness or adapter changes. Anything the client needs from them is a separate change.

## 2. Layout and dependencies

```
client/
  applewin_client/
    __init__.py       __version__ = "0.1.0"
    __main__.py       python -m applewin_client
    cli.py            argument parsing, main()
    bridge.py         the MCP side: start the adapter, list tools, call tools, convert results
    loop.py           the Claude side: the request, the turn loop, stop conditions
    transcript.py     the run directory: transcript.jsonl, images, summary
    prompts.py        the system prompt text
  requirements.txt    anthropic==<exact>, mcp==2.3.0
  README.md
tests/client/
  test_bridge.py  test_loop.py  test_transcript.py  test_integration.py  test_live.py
```

- Python 3.11 or later, like the adapter. `anthropic` is pinned to the exact version current when implementation starts. That version must accept `context_management` and `fallbacks` on `client.beta.messages.create`; check this against the installed SDK, not from memory. `mcp` is pinned to 2.3.0, the version the adapter is tested with.
- The client imports nothing from `applewin_mcp`. It talks to the adapter only over MCP stdio, as any client would.
- Every source file starts with `# SPDX-License-Identifier: MIT` (D6). `THIRD-PARTY-NOTICES.txt` gains `anthropic` (MIT).

## 3. Command line

```
python -m applewin_client --goal TEXT | --goal-file PATH
                          [--model claude-opus-5-5] [--effort low|medium|high|xhigh|max]
                          [--max-turns 200] [--max-tokens 16000]
                          [--clear-trigger 40000] [--clear-keep 6] [--clear-at-least 10000] [--no-clear]
                          [--run-dir PATH] [--python PATH]
                          -- <adapter arguments>
```

| Option | Default | Meaning |
|---|---|---|
| `--goal` / `--goal-file` | required, exactly one | The task, sent as the first user message. |
| `--model` | `claude-opus-5-5` | Model ID, passed through unchanged. |
| `--effort` | `medium` | `output_config.effort`. Sent explicitly, because the model's default may change. |
| `--max-turns` | 200 | Upper limit on API requests in the run. |
| `--max-tokens` | 16000 | `max_tokens` per request. |
| `--clear-trigger` | 40000 | Context editing `trigger`, in input tokens. |
| `--clear-keep` | 6 | Context editing `keep`, in tool uses. Must be at least 1 (D15: the agent must still hold the last image). |
| `--clear-at-least` | 10000 | Context editing `clear_at_least`, in input tokens. |
| `--no-clear` | off | Send no `context_management`. Used for A/B measurement. |
| `--run-dir` | `runs/<UTC timestamp>` | Where the transcript goes. Must not exist yet; the client creates it. |
| `--python` | `sys.executable` | The interpreter that runs the adapter. |
| after `--` | none | Passed to `python -m applewin_mcp` unchanged (`--exe`, `--disk`, `--realtime`, `--headless`, `--text-screen`, ...). |

Exit codes: 0 when the agent ended its turn with no tool call, 1 when a limit stopped the run (`--max-turns`, refusal, `max_tokens`), 2 on a usage error or when the adapter or the API failed.

## 4. The MCP side (`bridge.py`)

```python
class Bridge:
    async def __aenter__(self) -> "Bridge"   # start the adapter, initialize the session
    async def __aexit__(self, *exc) -> None  # close the session; the adapter shuts its harness down
    async def tools(self) -> list[dict]      # Claude tool definitions
    async def call(self, name: str, arguments: dict) -> tuple[list[dict], bool]   # (content blocks, is_error)
```

- **Start.** Launch the adapter with `StdioServerParameters(command=<python>, args=["-m", "applewin_mcp", *adapter_args])` and `mcp.client.stdio.stdio_client`, then `ClientSession(...).initialize()`. The tests in `tests/adapter/test_mcp.py` already use this pattern. The adapter's stderr goes to `<run-dir>/adapter.log`.
- **Tools.** Each MCP tool becomes `{"name": t.name, "description": t.description, "input_schema": t.inputSchema}`. They are listed once at start and kept in MCP's order, so the request prefix stays byte-identical and cacheable (§5.3).
- **Results.** Each MCP content item becomes one Claude content block:
  - `TextContent` → `{"type": "text", "text": ...}`
  - `ImageContent` → `{"type": "image", "source": {"type": "base64", "media_type": <mimeType>, "data": <data>}}`
  - Anything else → a text block `"[unsupported MCP content: <type>]"`.
  - An empty result becomes one text block `"(no output)"`, because the API rejects an empty `tool_result`.
  - `isError` becomes `is_error`.
- **Failures.** A tool error is data, not a failure: it goes back to the agent as `is_error: true`. If the MCP session itself fails (the adapter exits, the pipe breaks), the run stops with exit code 2. The message names the tool call that was running and points at `adapter.log`.

## 5. The Claude side (`loop.py`)

### 5.1 The request
Every request has this shape. Only `messages` grows from one request to the next.

```python
client.beta.messages.create(
    model=args.model,
    max_tokens=args.max_tokens,
    system=SYSTEM_PROMPT,
    tools=bridge_tools,
    messages=messages,
    thinking={"type": "adaptive"},
    output_config={"effort": args.effort},
    cache_control={"type": "ephemeral"},
    context_management={"edits": [{
        "type": "clear_tool_uses_20250919",
        "trigger": {"type": "input_tokens", "value": args.clear_trigger},
        "keep": {"type": "tool_uses", "value": args.clear_keep},
        "clear_at_least": {"type": "input_tokens", "value": args.clear_at_least},
    }]},                                    # left out with --no-clear
    fallbacks="default",
    betas=["context-management-2025-06-27", "server-side-fallback-2026-07-01"],
)
```

- **Non-streaming.** One agent turn is short and `max_tokens` is 16000, so it stays within the SDK's HTTP timeout. That also removes partial tool-input handling.
- **Thinking.** On Claude Opus 5.5, thinking cannot be disabled. `effort` is the only control.
- **No `tool_choice`.** Forced tool choice returns a 400 on this model. The default is `auto`.
- **No sampling parameters** (`temperature`, `top_p`, `top_k`). They return a 400.
- **`fallbacks: "default"`.** A request the safety classifiers decline is re-run server-side on a fallback model. A final `stop_reason: "refusal"` means the whole chain declined (§5.4).
- **Credentials** come from the SDK's default resolution (`ANTHROPIC_API_KEY`, then the `ant auth login` profile). The client never reads, prints or logs a key.

### 5.2 The loop
```
messages = [{"role": "user", "content": goal}]
for turn in 1..max_turns:
    response = create(...)
    messages.append({"role": "assistant", "content": response.content})   # verbatim, never edited
    log the turn (§6)
    match response.stop_reason:
        "end_turn" with no tool_use block   → finish, exit 0
        "tool_use"                          → run every tool_use block in order, one at a time;
                                              append ONE user message holding all their tool_result blocks
        "max_tokens"                        → stop, exit 1 (the turn was cut off)
        "refusal"                           → stop, exit 1 (log stop_details)
        anything else                       → stop, exit 1 (log it)
stop at max_turns, exit 1
```

- **Append-only history.** Request N's `messages` must start with request N−1's `messages`, element for element. The response content (thinking blocks included) is appended exactly as received, and nothing earlier is ever changed, removed or reordered. Clearing old tool results is the server's job (`context_management`), never the client's. This is what keeps preserved thinking valid and the prompt cache warm.
- **Tool calls run one at a time, in order.** The adapter holds one lock on the emulator anyway, and a later call (`look`) may depend on an earlier one (`press_keys`) having finished.
- **All tool results go back in one user message**, in the same order as the `tool_use` blocks, each with its `tool_use_id`.
- **SDK errors.** The SDK already retries 408, 409, 429, 5xx and connection errors (`max_retries` 2). After that, any `anthropic.APIStatusError` or `APIConnectionError` stops the run with exit code 2, and the error type, status and request id are logged. Catch the SDK's typed errors most specific first; don't match on message text.

### 5.3 Caching
`cache_control` at the top level caches the stable prefix (tools, system, and history up to the latest turn). The system prompt and tool list never change during a run, so nothing in them may vary: no timestamps, no run ids, no unsorted data. A clearing invalidates the cached prefix after the point it clears. `clear_at_least` is there so each clearing removes enough to pay for the new cache write.

### 5.4 Stop details
On `refusal`, log `response.stop_details` (`category`, `explanation`). On every response, log `response.model`, because a fallback serves the request with another model and later requests may stay with it.

## 6. The run directory (`transcript.py`)

```
<run-dir>/
  run.json          inputs: goal, model, effort, limits, clear settings, adapter args, client and adapter versions
  transcript.jsonl  one JSON object per line, in order
  images/           every image a tool returned, as <turn>-<n>.png
  adapter.log       the adapter's stderr
  summary.json      written at the end, including after an error
```

Records in `transcript.jsonl`:
- `{"kind": "response", "turn", "model", "stop_reason", "stop_details", "usage", "applied_edits", "content"}`
  - `usage` holds `input_tokens`, `cache_creation_input_tokens`, `cache_read_input_tokens` and `output_tokens`, copied as given.
  - `applied_edits` is `response.context_management.applied_edits`, or `[]`.
  - In `content`, thinking blocks are recorded as `{"type": "thinking", "chars": <len>}`. Their text is usually empty, and the signature is not useful in a log.
- `{"kind": "tool", "turn", "name", "input", "is_error", "content"}`. An image in `content` is recorded as `{"type": "image", "file": "images/<turn>-<n>.png", "sha256": ...}`, never as base64.
- `{"kind": "error", "turn", "message"}`

`summary.json` holds `{"turns", "exit_code", "reason", "totals": {input_tokens, cache_creation_input_tokens, cache_read_input_tokens, output_tokens}, "images": <count>, "clearings": <count>, "cleared_input_tokens": <sum>}`. The client prints the same totals on stdout at the end, and no money figure, because prices change.

## 7. The system prompt (`prompts.py`)

A fixed text, the same in every run. It says:
- You are operating an Apple IIe through the tools. You see the screen and hear sound only through them, and your input reaches the machine only through them.
- Work toward the user's goal. When it is done, or you are sure it cannot be done, stop calling tools and say so, in one short paragraph saying what you did.
- Images are expensive in your context. Use `look` with a region when you know where to look, zoom 1 for a rough look, and `look: false` on `wait_until_change` when you only need to know that something changed. A note saying the screen is the same as the last image means you already have it. Older tool results may be cleared from your context; call `look` again (with `force: true` if needed) rather than relying on an image from long ago.

It says nothing about the harness, the operator connection or checkpoints beyond what the tool list shows (D4).

## 8. Tests

**Unit tests** (no adapter, no API; `FakeSession` and `FakeAnthropic` stand-ins):
1. `tools()` maps the name, description and `inputSchema` exactly, in MCP order.
2. `call()` converts text, image, mixed, empty and error results as in §4.
3. The loop finishes with exit 0 on `end_turn` with no tool use.
4. The loop runs two `tool_use` blocks in order and sends both results in one user message, in order, with matching ids.
5. Append-only: across a scripted five-turn run, every request's `messages` starts with the previous request's.
6. The request carries `context_management` with the flag values, and leaves it out with `--no-clear`. It also carries `thinking`, `output_config.effort`, `fallbacks` and both betas, and no `tool_choice`, `temperature`, `top_p` or `top_k`.
7. `refusal`, `max_tokens` and an unknown stop reason each stop with exit 1 and a transcript record.
8. `--max-turns 3` stops after exactly 3 requests.
9. A failure of the MCP session stops with exit 2, and `summary.json` is still written.
10. Transcript: images are written as files and recorded by path and sha256; no record holds base64 data; no record or log holds the value of `ANTHROPIC_API_KEY` (set it to a known dummy in the test).
11. `--clear-keep 0`, both `--goal` and `--goal-file`, and an existing `--run-dir` are usage errors (exit 2).

**Integration test** (real adapter and harness, scripted model, no API cost). `FakeAnthropic` scripts three turns: `look`, then `type_text` with `"PRINT 2+2\n"`, then `end_turn`. Run it against the adapter with `--headless --disk` on the DOS 3.3 System Master (the fixture the adapter tests use). Assert:
- the first tool result holds an image block with `media_type` `image/png`;
- `images/1-1.png` decodes as a PNG;
- the run exits 0.

Skipped when `HARNESS_EXE` is not set, like the adapter tests.

**Live smoke test** (real API, costs money). It runs only when `APPLEWIN_LIVE_SMOKE=1`, and only after the user has approved the spend. Goal: "Type PRINT 2+2 and tell me what the computer answered." Run it with `--clear-trigger 3000 --clear-keep 2 --max-turns 15`. Check:
- the run exits 0 and the final text contains `4`;
- at least one `applied_edits` entry appears;
- no request fails because of the clearing.

This is where the two open points in §9 are checked.

## 9. Open points for review

1. **Clearing and preserved thinking.** The context-editing docs don't say whether clearing tool results interacts with Opus 5.5's check that the history was not edited. The client stays append-only, so it should be fine; the live smoke test confirms it. If it fails, the fallback is `--no-clear` plus a shorter run.
2. **Images in cleared results.** The docs don't say whether clearing removes image blocks inside a `tool_result`. The smoke test compares `cleared_input_tokens` with the images' visual-token cost to tell.
3. **Defaults.** `--clear-trigger 40000`, `--clear-keep 6` and `--clear-at-least 10000` are first guesses: a screenshot is 280 tokens, and the tool list is several thousand. Tune them from measured runs (A/B against `--no-clear`).
4. **Model.** The default is `claude-opus-5-5`. A cheaper model for long play sessions is a `--model` choice, not a code change.

## 10. Done when
- Every test in §8 except the live smoke passes on CI (`python -m unittest discover -s tests/client -v`).
- `client/README.md` shows one complete command line and explains the run directory.
- `THIRD-PARTY-NOTICES.txt` lists `anthropic`.
- The live smoke test has been run once with the user's approval, and its `summary.json` is attached to the PR.
