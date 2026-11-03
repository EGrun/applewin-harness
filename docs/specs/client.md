# Agent client spec

**Status:** Proposed (D15–D19). For review before implementation. Revision 4 (2026-11-02):
- revision 2 made the client a chat session;
- revision 3 gave the agent a persistent notebook (§6);
- revision 4 adds campaigns, which carry the machine, the disks and the notebook between sessions (§7, with `docs/specs/persistence.md`), a shell (§8) and the person's manuals (§9).

## 1. Purpose

`applewin_client` is a terminal chat program, in the manner of Claude Code, that talks to one Claude agent operating the AppleWin MCP adapter (`adapter/applewin_mcp`). You give it a goal, watch it work, send it messages while it works, interrupt it when it is off track, and give it the next goal when it is done. It exists so that the agent loop is ours, which the off-the-shelf MCP clients (Claude Desktop, Claude Code) do not allow:

- **You can steer it.** A message typed while the agent works reaches it at its next step. An interrupt stops it within a moment (D16).
- **It continues where it stopped.** One campaign per program keeps the agent's notebook of facts, open hypotheses, progress and dead ends (D17), the disks with the game's saves, and in stepped sessions the machine itself (D18). A goal too long or too hard for one context or one session goes on in the next.
- **It can compute and read.** A sandboxed shell runs bash and code for puzzles, routes and arithmetic, and the person's manuals are on its shelf (D19).
- **Old screenshots stop costing tokens.** The client asks the Claude API to clear old tool results with context editing (D15). Every image otherwise stays in context for the whole session.
- **Sessions are measured.** Every turn's token usage, cache use and clearing is logged, so the effect of D15 can be checked with numbers.
- **Scripted runs are repeatable.** `--once` runs one goal without a terminal and exits with a status code. The goal, model, effort, adapter flags and the campaign state it started from are all recorded.

### Non-goals
- No tools beyond the adapter's, except the client's own: memory (§6), the shell (§8) and the manuals (§9). None of them touches the emulated machine. The client does not filter the adapter's tools (the adapter decides what the agent may do, D4). The human's messages are chat, not a new power over the machine.
- No full-screen interface, no mouse, no inline images (a terminal cannot show the screen; §10 saves every image and `/show` opens it).
- No resuming a closed session's conversation (§15). The campaign is what carries over.
- No multi-agent orchestration, no other model providers.
- No harness or adapter changes in this spec. What the client needs from them is specified in `docs/specs/persistence.md`.

## 2. Layout and dependencies

```
client/
  applewin_client/
    __init__.py       __version__ = "0.4.0"
    __main__.py       python -m applewin_client
    cli.py            argument parsing, main(); picks the front end
    campaign.py       the campaign directory: name, lock, layout, startup facts (§7)
    bridge.py         the MCP side: start the adapter, list tools, call tools, convert results
    conversation.py   the Claude side: the request, the turn loop, steering, interruption (no terminal code)
    memory.py         the notebook: the memory tool's commands on a directory (§6)
    shell.py          the code execution container: reuse, uploads (§8)
    manuals.py        the manuals tool: list, search, read (§9)
    chat.py           the interactive front end: prompt, key bindings, rendering of events, slash commands
    plain.py          the --once front end: prints events, returns an exit code
    transcript.py     the session directory: transcript.jsonl, images, summary
    prompts.py        the system prompt text
  requirements.txt    anthropic==<exact>, mcp==2.3.0, prompt_toolkit==<exact>, pypdf==<exact>
  README.md
tests/client/
  test_bridge.py  test_conversation.py  test_campaign.py  test_memory.py  test_shell.py  test_manuals.py
  test_chat.py  test_transcript.py  test_integration.py  test_live.py
```

- Python 3.11 or later, like the adapter. `anthropic` is pinned to the exact version current when implementation starts. That version must accept `context_management`, `fallbacks` and `container` on `client.beta.messages.stream`, and provide the Files API; check this against the installed SDK, not from memory. `mcp` is pinned to 2.3.0, the version the adapter is tested with. `prompt_toolkit` is pinned to the exact current version; it is the one library that gives a live input line under streaming output on Windows, macOS and Linux (D16). `pypdf` reads page counts and text layers and cuts page ranges out of a PDF (§9).
- The client imports nothing from `applewin_mcp`. It talks to the adapter only over MCP stdio, as any client would.
- Every source file starts with `# SPDX-License-Identifier: MIT` (D6). `THIRD-PARTY-NOTICES.txt` gains `anthropic` (MIT), `prompt_toolkit` (BSD-3-Clause) and `pypdf` (BSD-3-Clause).
- `conversation.py` knows nothing about the terminal. It reports what happens through an event sink (§5.5), so the chat front end, the `--once` front end and the tests all drive the same code.
- `.gitignore` gains `runs/` and `campaigns/`.

## 3. Command line

```
python -m applewin_client [--goal TEXT | --goal-file PATH] [--once]
                          [--campaign NAME|PATH] [--no-memory] [--fresh-machine] [--fresh-disks]
                          [--manuals DIR] [--no-shell]
                          [--model claude-opus-5-5] [--effort low|medium|high|xhigh|max]
                          [--max-turns 200] [--max-tokens 16000]
                          [--clear-trigger 40000] [--clear-keep 6] [--clear-at-least 10000] [--no-clear]
                          [--run-dir PATH] [--python PATH]
                          -- <adapter arguments>
```

| Option | Default | Meaning |
|---|---|---|
| `--goal` / `--goal-file` | none | The first message. Without one, the chat starts at an empty prompt. At most one of the two. |
| `--once` | off | Send the goal, run until the agent finishes or a limit stops it, print the totals and exit. No terminal needed. Requires a goal. |
| `--campaign` | the stem of the adapter's `--disk`, else `default` | The campaign (§7). A bare name means `campaigns/<name>/`; a value containing a path separator is a directory. |
| `--no-memory` | off | No memory tool and no notebook for this session. The machine and disks still carry over. |
| `--fresh-machine` | off | Passed to the adapter: boot fresh even if the last stepped session's end state could be restored (persistence spec §5). |
| `--fresh-disks` | off | Passed to the adapter: move the campaign's saved disks to its archive and start from the original images (persistence spec §4). |
| `--manuals` | the campaign's remembered folder, else none | A folder of the person's manuals (§9). The folder is remembered in the campaign, so later sessions need not repeat it. |
| `--no-shell` | off | No code execution tool (§8). |
| `--model` | `claude-opus-5-5` | Model ID, passed through unchanged. Fixed for a conversation; `/new` starts one with `--model` again. |
| `--effort` | `medium` | `output_config.effort`. Sent explicitly, because the model's default may change. `/effort` changes it for later requests (the cache restarts; thinking stays valid). |
| `--max-turns` | 200 | Upper limit on API requests for one user message, goal or steering message that starts a run. |
| `--max-tokens` | 16000 | `max_tokens` per request. |
| `--clear-trigger` | 40000 | Context editing `trigger`, in input tokens. |
| `--clear-keep` | 6 | Context editing `keep`, in tool uses. Must be at least 1. It counts tool uses, not images, so it does not guarantee the last image survives (D15); the agent recovers with `look` and `force: true`. Memory and manuals calls count too. |
| `--clear-at-least` | 10000 | Context editing `clear_at_least`, in input tokens. |
| `--no-clear` | off | Send no `context_management`. Used for A/B measurement. |
| `--run-dir` | `runs/<UTC timestamp>` | Where the session goes. Must not exist yet; the client creates it. |
| `--python` | `sys.executable` | The interpreter that runs the adapter. |
| after `--` | none | Passed to `python -m applewin_mcp` unchanged (`--exe`, `--disk`, `--realtime`, `--headless`, `--text-screen`, ...). The client adds `--state-dir`, `--fresh-machine` and `--fresh-disks` itself; giving `--state-dir` here is a usage error. |

Interactive mode needs a terminal on stdin and stdout; without one and without `--once`, the client exits with code 2 and says so.

Exit codes. `--once`: 0 when the agent ended its turn with no tool call, 1 when a limit stopped it (`--max-turns`, refusal, `max_tokens`), 2 on a usage error or when the adapter or the API failed. Interactive: 0 on a normal quit, 2 on a usage error or when the adapter or the API failed. A limit that stops one message in the chat is shown and the chat goes on.

## 4. The MCP side (`bridge.py`)

```python
class Bridge:
    async def __aenter__(self) -> "Bridge"   # start the adapter, initialize the session
    async def __aexit__(self, *exc) -> None  # close the session; the adapter saves disks and state, then shuts its harness down
    async def tools(self) -> list[dict]      # Claude tool definitions
    async def call(self, name: str, arguments: dict) -> tuple[list[dict], bool]   # (content blocks, is_error)
```

- **Start.** Launch the adapter with `StdioServerParameters(command=<python>, args=["-m", "applewin_mcp", *adapter_args, "--state-dir", <campaign>/machine, *fresh_flags])` and `mcp.client.stdio.stdio_client`, then `ClientSession(...).initialize()`. The tests in `tests/adapter/test_mcp.py` already use this pattern. The adapter's stderr goes to `<run-dir>/adapter.log`; lines at `WARNING` or above (for example a failed disk save, persistence spec §4) are also shown as notices. The adapter lives for the whole session, so the emulated machine keeps its state from one goal to the next.
- **Tools.** Each MCP tool becomes `{"name": t.name, "description": t.description, "input_schema": t.inputSchema}`. They are listed once at start and kept in MCP's order, so the request prefix stays byte-identical and cacheable (§5.3). If the adapter ever lists a tool named `memory`, `manuals` or `code_execution`, the client stops with exit 2 rather than shadow it.
- **Results.** Each MCP content item becomes one Claude content block:
  - `TextContent` → `{"type": "text", "text": ...}`
  - `ImageContent` → `{"type": "image", "source": {"type": "base64", "media_type": <mimeType>, "data": <data>}}`
  - Anything else → a text block `"[unsupported MCP content: <type>]"`.
  - An empty result becomes one text block `"(no output)"`, because the API rejects an empty `tool_result`.
  - `isError` becomes `is_error`.
- **Failures.** A tool error is data, not a failure: it goes back to the agent as `is_error: true`. If the MCP session itself fails (the adapter exits, the pipe breaks), the session stops with exit code 2. The message names the tool call that was running and points at `adapter.log`.
- **A tool call is never abandoned.** The adapter runs one call at a time under a lock, and a call can take emulated seconds. The client waits for a call it has started to return; an interrupt (§5.4) takes effect after it.
- **Shutdown.** Closing the session is a normal adapter shutdown, which saves the disks and, in a stepped session, the end state (persistence spec §5). The client waits for the adapter to exit, up to 60 s, before it reports the session ended; if it has to kill it, the notice says the end state was not saved.

## 5. The Claude side (`conversation.py`)

### 5.1 The request
Every request has this shape. Only `messages` grows from one request to the next.

```python
async with client.beta.messages.stream(
    model=settings.model,
    max_tokens=settings.max_tokens,
    system=SYSTEM_PROMPT,
    tools=bridge_tools
          + [{"type": "memory_20250818", "name": "memory"}]              # left out with --no-memory
          + [MANUALS_TOOL]                                               # only with a manuals folder (§9)
          + [{"type": "code_execution_20260521", "name": "code_execution"}],   # left out with --no-shell
    container=shell.container_id,           # the campaign's container (§8); left out with --no-shell or when there is none yet
    messages=messages,
    thinking={"type": "adaptive"},
    output_config={"effort": settings.effort},
    cache_control={"type": "ephemeral"},
    context_management={"edits": [{
        "type": "clear_tool_uses_20250919",
        "trigger": {"type": "input_tokens", "value": settings.clear_trigger},
        "keep": {"type": "tool_uses", "value": settings.clear_keep},
        "clear_at_least": {"type": "input_tokens", "value": settings.clear_at_least},
    }]},                                    # left out with --no-clear
    fallbacks="default",
    betas=["context-management-2025-06-27", "server-side-fallback-2026-07-01"],   # plus whatever the code execution docs require (§15)
) as stream:
    ...                                     # text deltas go to the sink; the final message is stream.get_final_message()
```

- **Streaming.** The chat needs text as it arrives, and an interrupt needs a request it can cancel in a moment. Tool inputs here are small, so `eager_input_streaming` is not set; the tool call runs from the final message, whose `input` is already parsed. If the stream fails before a final message, nothing is appended (§5.2).
- **Tool dispatch.** A `tool_use` named `memory` goes to `memory.py` (§6), `manuals` to `manuals.py` (§9), and every other name to the bridge. `code_execution` is a server tool: the API runs it in Anthropic's container and returns its result in the same response, so the client never runs it.
- **Thinking.** On Claude Opus 5.5, thinking cannot be disabled. `effort` is the only control. Thinking text is not shown; the sink gets a "thinking" event so the display can show that the agent is working.
- **No `tool_choice`.** Forced tool choice returns a 400 on this model. The default is `auto`.
- **No sampling parameters** (`temperature`, `top_p`, `top_k`). They return a 400.
- **`fallbacks: "default"`.** A request the safety classifiers decline is re-run server-side on a fallback model. A final `stop_reason: "refusal"` means the whole chain declined (§5.6).
- **Credentials** come from the SDK's default resolution (`ANTHROPIC_API_KEY`, then the `ant auth login` profile). The client never reads, prints or logs a key.

### 5.2 The conversation and the turn loop

```python
class Conversation:
    def __init__(self, client, bridge, memory, manuals, shell, settings, recorder, sink): ...   # memory, manuals, shell may be None
    async def send(self, text: str) -> Outcome     # add a user message and run turns until the agent stops
    def steer(self, text: str) -> None             # queue a message for the next boundary; safe to call while send() runs
    def interrupt(self) -> None                    # ask the running send() to stop at the next safe point
    @property
    def running(self) -> bool
```

`Outcome.reason` is one of `done`, `interrupted`, `max_turns`, `max_tokens`, `refusal`, `error`. `send` is never called while another is running; the front end routes a message typed during a run to `steer`.

```
send(text):
    append {"role": "user", "content": [startup note if first message of the process (§7.3)] + text + [container uploads (§8)]}
                                                      (merged with the last message if that is a user message, §5.4)
    for turn in 1..max_turns:
        response = stream a request
        if interrupted during the request: drop the partial response, stop (reason interrupted)
        messages.append({"role": "assistant", "content": response.content})   # verbatim, never edited
        record and report the turn; remember response.container (§8)
        match response.stop_reason:
            "end_turn" with no client tool_use block:
                if a steering message is queued: append it as a user message, continue
                else: stop (reason done)
            "tool_use":
                run each client tool_use block, one at a time, in order
                    (if interrupted before one starts: its result is an error "not run: the user interrupted")
                append ONE user message: all tool_result blocks in order, then one text block per queued steering message
                if interrupted: stop (reason interrupted)
            "pause_turn":
                (a server tool turn was paused) send the request again as it is, with nothing appended, and continue
            "max_tokens" → stop (the turn was cut off)
            "refusal"    → stop (record stop_details)
            anything else → stop (record it)
    stop at max_turns
```

- **Append-only history.** Request N's `messages` must start with request N−1's `messages`, element for element. The response content (thinking blocks and server tool blocks included) is appended exactly as received, and nothing earlier is ever changed, removed or reordered. Clearing old tool results is the server's job (`context_management`), never the client's. This is what keeps preserved thinking valid and the prompt cache warm. Dropping a response that was never complete (an interrupt, a failed stream) is not an edit: it was never appended.
- **Tool calls run one at a time, in order.** The adapter holds one lock on the emulator anyway, and a later call (`look`) may depend on an earlier one (`press_keys`) having finished. Memory and manuals calls run in the same order as the others.
- **All tool results go back in one user message**, in the same order as the client `tool_use` blocks, each with its `tool_use_id`. Text blocks (steering) come after every `tool_result`, as the API requires. Server tool blocks need no result from the client.
- **SDK errors.** The SDK already retries 408, 409, 429, 5xx and connection errors (`max_retries` 2). After that, any `anthropic.APIStatusError` or `APIConnectionError` ends the `send` with `Outcome.reason` `error` (the type, status and request id are recorded), and the chat goes on at the prompt; `--once` exits 2. Catch the SDK's typed errors most specific first; don't match on message text. An error saying the container no longer exists is handled by §8, not here.

### 5.3 Caching
`cache_control` at the top level caches the stable prefix (tools, system, and history up to the latest turn). The system prompt and tool list never change during a conversation, so nothing in them may vary: no timestamps, no session ids, no unsorted data, nothing from the notebook and no list of manuals (the agent reads both through their tools, never through the system prompt). A clearing invalidates the cached prefix after the point it clears. `clear_at_least` is there so each clearing removes enough to pay for the new cache write. Messages typed by the human, the startup note and container uploads arrive in user messages after the cached prefix and do not disturb it.

### 5.4 Steering and interruption
- **Steering.** A message typed while the agent works is shown at once (dimmed, marked "queued") and delivered at the next boundary: in the user message that carries the tool results, or as a new user message when the agent has just ended its turn. The delay is one request plus one tool call. After an `end_turn`, a queued message makes the loop continue instead of stopping.
- **Interrupt.** Esc, or Ctrl+C once, while the agent works:
  1. A request in flight is cancelled and its partial response is dropped. Code the container had already started may have run; its files stay in the container.
  2. A client tool call in flight is waited for. Client tool calls not yet started are answered with the error result above, so every client `tool_use` in the history has a `tool_result`.
  3. The `send` ends with reason `interrupted`, and the prompt returns.
  The next message the human types is an ordinary new message. When the history already ends in a user message (the tool results, or the dropped request's input), the new message becomes a further user message after it, and the API combines consecutive user turns. The integration test checks that the live API accepts this; if it does not, the fallback is to put the text into the same user message as a new content block, which edits the last message only and nothing before it.
  Ctrl+C twice, or once while idle with an empty line, quits (§11).
- **What an interrupt does not do.** It does not pause the emulator; the machine is wherever the last completed tool call left it. A realtime session keeps running while the human types. (The agent has no pause either, by the agent-power rules.) A stepped session stands still between calls. A memory write that has started completes, and is atomic (§6.1), so an interrupt never leaves a half-written note.

### 5.5 Events (the sink)
`conversation.py` reports through a sink with these methods, called from the event loop:

| Event | Arguments |
|---|---|
| `text(delta)` | assistant text as it streams |
| `thinking()` | the agent is thinking (no text) |
| `tool_started(name, input)` | before a client tool call |
| `tool_finished(name, content, is_error, images)` | `images` lists the saved files and sizes (§10) |
| `server_tool(name, input, result)` | a code execution call and its output, from the response |
| `turn_done(turn, usage, applied_edits, model)` | after each response |
| `steering_delivered(text)` | when a queued message goes into the history |
| `outcome(outcome)` | when `send` ends |
| `notice(text)` | clearing happened, model changed by a fallback, machine restored or booted fresh (§7.3), notebook edited outside the client (§6.4), a new container (§8), an adapter warning, and similar |

The chat front end renders them; the `--once` front end prints them plainly; tests collect them in a list.

### 5.6 Stop details
On `refusal`, record `response.stop_details` (`category`, `explanation`) and show it. On every response, record `response.model`, because a fallback serves the request with another model and later requests may stay with it; a change is shown as a `notice`.

### 5.7 Long conversations
Context editing keeps screenshots from piling up, but a long chat still grows by every message, thinking block and text. The status line (§11) shows the last request's input tokens against the model's context window. v1 does not compact: when the input approaches the window the client says so, and `/new` starts a fresh conversation on the same emulator with an empty history. With the campaign (§6, §7), `/new` loses little: the agent reads its notebook first and carries on. Compaction is an open point (§15).

## 6. Memory (`memory.py`)

The agent keeps a notebook that outlives the conversation: what it has verified, what it suspects and still has to test, what it is doing next, and what it tried that failed. It reads it at the start of every conversation and writes to it as it works, so a goal that spans many sessions, a `/new`, a crash or a long run of cleared context continues where it left off (D17).

### 6.1 The tool and its handler
- **The tool** is Anthropic's memory tool, `{"type": "memory_20250818", "name": "memory"}`, listed after the adapter's tools. It is client-side and generally available: no beta header and no input schema. When it is present the API adds its own memory protocol to the system prompt (view the memory directory before anything else, record progress as you go, assume your context may be reset at any moment), so `prompts.py` does not repeat it. Source: the memory tool docs, platform.claude.com/docs/en/agents-and-tools/tool-use/memory-tool, read 2026-11-02.
- **The handler** is `class MemoryStore` in `memory.py`, standard library only:

  ```python
  class MemoryStore:
      def __init__(self, root: Path) -> None            # the directory /memories maps onto; created if missing
      def run(self, command: dict) -> tuple[str, bool]   # one memory command → (result text, is_error)
      def listing(self) -> list[dict]                    # [{"path", "bytes", "sha256"}] of every visible file, sorted
  ```

  It runs `view`, `create`, `str_replace`, `insert`, `delete` and `rename` with the inputs and return strings in the memory tool docs, and returns errors with `is_error: true`. The SDK's `BetaLocalFilesystemMemoryTool` is built for the SDK's tool runner, which this client does not use, and lacks the limits below, so the client has its own handler.
- **Paths.** `/memories` maps to the store root, `<campaign>/notebook/`. Every path in every command is checked before anything is touched:
  - it is `/memories` or starts with `/memories/`, after percent-decoding;
  - no segment is `..` or starts with `.` (hidden files belong to the client, §6.4); no backslash, drive letter, colon or NUL;
  - `Path.resolve()` of the mapped path is inside the resolved root (`relative_to`), which also rejects a symlink that leads out.
  A path that fails gets `Error: The path {path} is not allowed; memory paths are under /memories`.
- **`create` does not overwrite.** An existing file gets the reference error, `Error: File {path} already exists`, so a notebook is changed by edits, not by a whole rewrite that can drop what it held. `delete` and `rename` refuse the root.
- **Limits.** A file holds at most 64 KiB of UTF-8 text, the store at most 1 MiB and 200 files. A write that would pass a limit is refused with an error that names the limit and asks the agent to condense or merge notes. A file `view` returns at most 16,000 characters and then says `(truncated: use view_range to read lines past {n})`.
- **Atomic writes.** Every write goes to a temporary file in the same directory and then `os.replace`. A failure leaves the old file as it was.
- **Recording.** Each memory call is a `tool` record in the transcript with its input and result, like any other tool. The chat shows it as one dimmed line (`▸ memory str_replace /memories/hypotheses.md`).

### 6.2 What the agent keeps
The layout is given in the system prompt (§13). The agent creates the files the first time it needs them; the client never seeds or writes notes.

- `/memories/README.md`: what the notebook is about (the disk and program), each goal so far with its status, and an index of the other files. Kept short; it is what the agent reads first.
- `/memories/facts.md`: what has been verified, each with its evidence: what was done, what the screen or sound showed, or which manual page says it, and when (the session directory name and turn). Facts about the program (controls, rules, maps, what a command does) stay true across sessions. Facts about the machine's state (where the character stands, which disk is in which drive) are marked as state and dated, because the next session may start from a fresh boot.
- `/memories/hypotheses.md`: theories still to be tested, one entry each:

  ```
  ## H<n>: <the claim>
  status: untested | testing | confirmed | refuted
  why: <what suggests it>
  test: <how to check it, cheapest test first>
  result: <what happened and when, once tested>
  ```

  Numbers are never reused. A confirmed hypothesis is marked confirmed and its claim becomes a fact in `facts.md` ("from H<n>"). A refuted one stays, marked refuted with its evidence, so it is not tried again.
- `/memories/progress.md`: the current plan, the next steps, the dead ends (what was tried, what happened, and why it did not work), and which checkpoints or in-game saves hold what (§7.2).
- `/memories/scripts/`: code from the shell (§8) worth keeping, as text, each with a line saying what it does and what it needs.
- Topic files as needed (`/memories/map-level-1.md`, `/memories/controls.md`), each listed in `README.md`.

### 6.3 Fair play
- The notebook is the agent's own. It holds what the agent saw through the adapter's tools, read in the manuals, or worked out in the shell, and what it concluded. The client writes nothing into it, and nothing in it comes from the harness, the operator connection, emulator memory or checkpoints' contents (D4). Memory gives the agent recall, not new senses.

### 6.4 Outside edits
- The person can read and edit the files: they are plain Markdown. An edit made between sessions is guidance, like a chat message, and is marked: at a normal end of session the client writes the final `listing()` to `<campaign>/notebook/.client-state.json` (hidden, so the agent cannot read or change it). At the next start, if the store's listing differs, the client records `"memory_edited_outside": true` in `run.json` and `summary.json` and shows a notice. A missing or unreadable state file (first use, a crash) counts as no edit known.
- A run meant as unaided uses `--once --no-memory`, or a fresh `--campaign`.

### 6.5 With clearing, `/new` and compaction
- A memory `view` result is a tool result and can be cleared like any other. That is the intended pattern: the agent reads a note when it needs it, and the context does not have to hold the whole notebook.
- Memory calls count toward `keep`, which makes the last screenshot more likely to be cleared; the agent recovers with `force` (D15).
- The notebook is what survives `/new`, a crash, the end of a session and, later, compaction (§15).

## 7. Campaigns (`campaign.py`)

A campaign is everything that carries over for one program: the notebook, the machine and its disks, the shell's container and the manuals folder.

### 7.1 Layout
```
campaigns/<name>/
  notebook/          the memory store (§6); .client-state.json is the client's (§6.4)
  machine/           the adapter's --state-dir (persistence spec §2): disks/, states/, last_session.json, startup.json, archive/
  shell.json         { "container_id", "expires_at", "uploads": [{ "file", "sha256", "file_id" }] } (§8)
  campaign.json      { "name", "created_at", "manuals_dir", "sessions": <count> }
  .lock              held by the client while a session runs
```

- **Name.** The default is the stem of the adapter's `--disk`, lowercased with every run of characters other than letters and digits replaced by `-` (`Ultima IV Disk 1.dsk` → `ultima-iv-disk-1`); without `--disk` it is `default`. Each program gets its own campaign, and two programs' notes, disks and states never mix.
- **One session at a time.** The client takes `.lock` (an exclusive OS lock) before starting the adapter, which takes its own lock on `machine/`. A second client on the same campaign exits with code 2 and names the campaign.
- `run.json` records the campaign path, the notebook's `listing()`, the persisted disks' index and the startup facts (§7.3) at the start; `summary.json` records the same at the end.

### 7.2 What carries over, by run mode
The rules are the user's (2026-11-02) and are implemented by the adapter (persistence spec):

| | Stepped session | Realtime session |
|---|---|---|
| Notebook | yes | yes |
| Disks, with every write the game made | yes | yes |
| The machine itself (end state restored at the next start) | yes, when nothing changed since (persistence spec §5) | no, it boots fresh |
| The agent's checkpoints | kept between sessions; the tools exist | not offered |
| Saving progress | checkpoints or the game's own save | only the game's own save, to disk |

The agent should know which checkpoint or in-game save holds what; that goes in `progress.md` (§6.2).

### 7.3 The startup note
After the adapter starts, the client reads `machine/startup.json` and puts one line, marked as the harness's, at the start of the first user message of the process (never into the system prompt, §5.3). It is one of:

The line follows `startup.json`'s `reason` (persistence spec §5), one line per code:

| `reason` | Line |
|---|---|
| `restored` | `[harness] The machine was restored to where the previous session ended (checkpoint <id>, <UTC time>).` (the time is `previous_ended_at`) |
| `realtime` | `[harness] This is a realtime session: the machine was started fresh. The disks hold every save made in earlier sessions.` |
| `fresh_machine` | `[harness] The machine was started fresh because --fresh-machine was given. The disks hold every save made in earlier sessions.` |
| `fresh_disks` | `[harness] The machine was started fresh with the original disks (--fresh-disks).` |
| `restore_failed` | `[harness] The machine was started fresh: restoring the previous session's state failed. The disks hold every save made in earlier sessions.` |
| `first_session` | `[harness] This is the first session of this campaign.` |
| `previous_unclean` | `[harness] The machine was started fresh: the previous session did not end cleanly. The disks hold every save made in earlier sessions.` |
| `previous_realtime` | `[harness] The machine was started fresh: the previous session was realtime. The disks hold every save made in earlier sessions.` |
| `no_end_state` | `[harness] The machine was started fresh: the previous session left no saved state. The disks hold every save made in earlier sessions.` |
| `other_build` | `[harness] The machine was started fresh: the previous session's state is from another build. The disks hold every save made in earlier sessions.` |
| `disks_changed` | `[harness] The machine was started fresh: a disk changed since the previous session ended. The disks hold every save made in earlier sessions.` |

The same line is shown as a notice. `/new` adds no note: the machine is the one the agent left, as the system prompt says.

## 8. The shell (`shell.py`)

The agent gets bash and code through Anthropic's code execution tool (D19): a sandboxed container on Anthropic's side with bash, Python 3.11 and its data libraries, no network, and no access to this computer.

- **Why this tool.** The container cannot reach the harness, the disks, the campaign or anything else on the person's machine, so it gives the agent nothing a player with paper and a calculator would not have, by construction. A client-side bash tool would need a sandbox of our own on Windows (the API hosts none for client tools) and a fairness filter around every command.
- **The tool** is `{"type": "code_execution_20260521", "name": "code_execution"}`. The model runs bash commands and edits files in the container; the API returns the results in the response (server tool blocks such as `bash_code_execution_tool_result`). The client records them and shows them as one line each (`▸ shell python3 route.py` and the first line of the output), but runs nothing itself.
- **One container per campaign.** The first response that uses the tool returns a container; the client stores its id and expiry in `shell.json` and passes it as `container` on every later request, in this session and later ones, so files and installed scripts persist. Containers last up to 30 days. When the API reports the container gone or expired, the client drops the id, retries the request without `container`, shows a notice, and the agent is told in the next tool results' user message: `[harness] The shell's container expired; a new one was started and its files are gone.` The notebook's `scripts/` (§6.2) is how code survives that.
- **Manuals in the container.** When a container is new, the client uploads each manual (§9) with the Files API (once per file; `shell.json` keeps `file_id` by sha256, so an unchanged file is not uploaded again) and adds a `container_upload` block for each to the next user message, so the agent can `grep` and process them in the shell. Nothing else is ever uploaded: no disk image, no notebook, no save state.
- **Cost.** Container time is billed by the hour beyond a free allowance; `summary.json` records the container id and the number of code execution calls. No money figure is shown.
- **`--no-shell`** leaves the tool and `container` out.

## 9. The manuals (`manuals.py`)

The person's manuals, reference cards and maps go in a folder; the agent reads them through a read-only tool (D19). A player had the manual next to the machine, so this is fair: commands, rules and copy-protection questions are what manuals are for.

- **The folder** is `--manuals DIR`, remembered in `campaign.json`. Files of these kinds are offered: `.txt`, `.md` (text), `.pdf` (pages), `.png`, `.jpg`, `.jpeg` (scans). Others are listed as unsupported. Subfolders are included; hidden files are not.
- **The tool** is a client tool with this definition (fixed text, so it caches):

  ```json
  { "name": "manuals",
    "description": "Read the manuals that came with the program: list them, search their text, or read pages or lines. Read-only.",
    "input_schema": { "type": "object", "required": ["command"], "properties": {
        "command": { "enum": ["list", "search", "read"] },
        "query":   { "type": "string", "description": "search: words to find (case-insensitive)" },
        "file":    { "type": "string", "description": "read: a path as list shows it" },
        "pages":   { "type": "string", "description": "read, PDF: a page or range such as \"3\" or \"3-5\" (at most 5 pages)" },
        "lines":   { "type": "string", "description": "read, text: a line range such as \"1-200\" (at most 400 lines)" } } } }
  ```

- **`list`** returns one line per file: its path relative to the folder, its kind, its size, and for a PDF its page count and whether it has a text layer.
- **`search`** looks in text files and PDF text layers and returns at most 20 hits as `file  p.<page>` or `file  l.<line>` with a snippet of about 160 characters. PDFs without a text layer are named at the end ("no text layer: read its pages").
- **`read`** returns:
  - a text file's lines with line numbers (default the first 400);
  - a PDF's pages as a `document` block holding a new PDF of just those pages (cut with `pypdf`), so scanned pages are read as images by the model; default the first page;
  - an image as an `image` block, scaled down to fit the API's image limits if needed.
- **Confinement.** Paths are relative to the folder; the same checks as the notebook's apply (§6.1: no `..`, no hidden segment, no absolute or drive path, `resolve()` inside the folder). The tool never writes.
- **Errors** are tool results with `is_error: true`: an unknown file, a range past the end, too many pages or lines, an unsupported kind.

## 10. The session directory (`transcript.py`)

```
<run-dir>/
  run.json          inputs: model, effort, limits, clear settings, adapter args, client and adapter versions, the goal if any,
                    the campaign (§7.1), the startup facts (§7.3), memory_edited_outside
  transcript.jsonl  one JSON object per line, in order
  images/           every image a tool returned, as <turn>-<n>.png (turn counts requests across the session)
  history           the chat prompt's input history
  adapter.log       the adapter's stderr
  summary.json      written at the end, including after an error and on every normal quit
```

Records in `transcript.jsonl`:
- `{"kind": "user", "turn", "text", "how"}`. `how` is `goal` (the `--goal` text), `message` (typed at an idle prompt), `steering` (typed while the agent worked, recorded when it is delivered), `harness` (the startup note, a container notice) or `command` (a slash command that changed something, e.g. `/new`, `/effort`).
- `{"kind": "response", "turn", "model", "stop_reason", "stop_details", "usage", "applied_edits", "container", "content"}`
  - `usage` holds `input_tokens`, `cache_creation_input_tokens`, `cache_read_input_tokens` and `output_tokens`, copied as given.
  - `applied_edits` is `response.context_management.applied_edits`, or `[]`.
  - In `content`, thinking blocks are recorded as `{"type": "thinking", "chars": <len>}`. Their text is usually empty, and the signature is not useful in a log. Code execution blocks are recorded with their command and output.
- `{"kind": "tool", "turn", "name", "input", "is_error", "content"}`. An image in `content` is recorded as `{"type": "image", "file": "images/<turn>-<n>.png", "sha256": ...}`, never as base64; a manuals `document` block as `{"type": "document", "file", "pages", "sha256"}`. Memory calls are recorded the same way, with their full input.
- `{"kind": "interrupted", "turn", "during"}` where `during` is `request` or `tool`.
- `{"kind": "error", "turn", "message"}`

`summary.json` holds `{"messages_from_user": {"goal", "message", "steering"}, "interrupts", "turns", "exit_code", "reason", "totals": {input_tokens, cache_creation_input_tokens, cache_read_input_tokens, output_tokens}, "images": <count>, "clearings": <count>, "cleared_input_tokens": <sum>, "memory": {"calls", "writes", "edited_outside", "listing_at_end"}, "machine": {"run_mode", "restored", "reason", "end_state_saved"}, "shell": {"container_id", "calls"}, "manuals": {"reads", "searches"}}`. The client prints the same totals on stdout at the end, and no money figure, because prices change. A session with steering, interrupts or an edited notebook is not comparable with an unaided run of the same goal, and these fields make that visible.

## 11. The interactive front end (`chat.py`)

A scrolling terminal, not a full-screen application. Output goes above a live prompt (`prompt_toolkit` with `patch_stdout`), so you can type while the agent's text streams.

```
  [harness] The machine was restored to where the previous session ended (checkpoint cp-41, 2026-11-02 21:55 UTC).
> Get the character to the second level and remember the way.
  (thinking)
  ▸ memory view /memories
  ▸ memory view /memories/progress.md
  My notes say the stairs are behind the locked door in the north room (H3, untested). The manual may say how keys work.
  ▸ manuals search "key"
  ▸ look
    [image 1-1  560×384  runs/…/images/1-1.png]
  ▸ press_keys  keys=["N","N"] …
> try the key from the chest first ┐ queued
  ...
 ctx 18.2k/200k · cache 91% · turns 7 · memory 5 files · stepped · working  (Esc interrupts)
>
```

- **Prompt.** Enter sends. Alt+Enter, or a trailing backslash, inserts a newline (messages may be several lines). Up and Down recall earlier messages, kept in `<run-dir>/history`. The prompt is always live: typing while the agent works queues a steering message (§5.4).
- **Rendering.** Assistant text streams as it arrives. Each tool call is one line (`▸ name key=value …`, long values truncated to the terminal width); its result is one line of the text, or `[image <turn>-<n> <w>×<h> <path>]` for an image, or `error: …`. Memory and manuals calls are one dimmed line with the command and path or query, and no result line unless it is an error. A shell call is one line with its command and the first line of its output. Thinking shows only as a dimmed `(thinking)` while it lasts. Colour is used only when the terminal supports it, and every line reads without it.
- **Status line.** One line under the output: the last request's input tokens against the context window, the cache hit share of that request, the turn count, the notebook's file count, the run mode, and `idle` or `working (Esc interrupts)`. It shows no money.
- **Keys.** Esc or Ctrl+C while working: interrupt (§5.4). Ctrl+C twice, or Ctrl+D on an empty line: quit. Ctrl+L: clear the screen (not the conversation).
- **Slash commands.** A line starting with `/` is a command, not a message. Unknown commands are reported and nothing is sent. They work at an idle prompt and while the agent works unless noted.

| Command | Does |
|---|---|
| `/help` | list the commands |
| `/new` | idle only. Start a fresh conversation (empty history) on the same emulator, adapter and campaign. The session directory keeps going; the transcript marks the boundary. |
| `/effort low\|medium\|high\|xhigh\|max` | change `effort` for later requests |
| `/usage` | print the session totals (the `summary.json` figures so far) |
| `/show [N]` | open the Nth most recent image (default the latest) in the system viewer (`os.startfile` on Windows, `open` on macOS, `xdg-open` elsewhere) |
| `/memory` | list the notebook's files and sizes, and the open hypotheses (`untested` or `testing`) by number and claim |
| `/memory open` | open the notebook's directory in the system file viewer |
| `/campaign` | show the campaign: its path, the run mode, what the machine started from (§7.3), the saved disks, the stepped checkpoints with their names, the manuals folder and the container's expiry |
| `/quit` | quit. A running `send` is interrupted first. |

There is no slash command for the model (a model change mid-conversation invalidates thinking and the cache; use `/new` after restarting with `--model`), for pausing the emulator, for anything that touches the machine, for restoring checkpoints, or for writing into the notebook. The person edits notes with their own editor, which §6.4 marks.

- **Quit.** The client interrupts any running `send`, closes the MCP session (the adapter saves the disks and, in a stepped session, the end state, then shuts its harness down; §4), writes the notebook state (§6.4), `shell.json` and `summary.json`, prints the totals and exits 0.

## 12. The `--once` front end (`plain.py`)

Prints the sink events as plain lines (no colour, no status line, no prompt), runs the one goal through `Conversation.send`, closes the adapter as on quit, writes the campaign state and the summary, prints the totals and exits with the code in §3. It is what the integration test, the live smoke test and any A/B measurement use.

## 13. The system prompt (`prompts.py`)

A fixed text, the same in every conversation with the same tools. It says:
- You are operating an Apple IIe through the tools. You see the screen and hear sound only through them, and your input reaches the machine only through them.
- A person is chatting with you. Work toward what they ask. When it is done, or you are sure it cannot be done, stop calling tools and say so, in one short paragraph saying what you did. They may send you a message while you work; treat it as an instruction from them, now. They may also give you a new goal after you finish; the machine is in the state you left it. A line starting `[harness]` is from the program running you, not from the person.
- Images are expensive in your context. Use `look` with a region when you know where to look, zoom 1 for a rough look, and `look: false` on `wait_until_change` when you only need to know that something changed. A note saying the screen is the same as the last image means you already have it, unless that image has been cleared from your context. If you get the note but no longer have the image, call `look` with `force: true`. Older tool results may be cleared from your context; call `look` again (with `force: true` if needed) rather than relying on an image from long ago.
- Progress outlives this conversation. The disks keep everything the program saves to them. In a stepped session your checkpoints are also kept for later sessions, and restoring one puts the disks back as they were then, undoing saves made since. In a realtime session the program's own save is the only way to keep progress.

With the memory tool present (no `--no-memory`), the prompt adds a fixed section:
- Your memory directory is your notebook for this program, kept across conversations. Keep it in this layout: `README.md` (what the notebook covers, each goal and its status, an index of files), `facts.md` (verified facts, each with what you did, what you saw or which manual page says it, and when), `hypotheses.md` (one `## H<n>: claim` entry per theory with `status`, `why`, `test` and `result`; never reuse a number), `progress.md` (plan, next steps, dead ends, and which checkpoint or in-game save holds what), `scripts/` (shell code worth keeping) and topic files listed in `README.md`.
- Record a fact as soon as you have verified it, and a hypothesis as soon as you form one you will not test at once. Test hypotheses cheapest first. When a test settles one, mark it confirmed (and add the fact) or refuted (with the evidence), so it is never tried again blindly.
- Before a long or risky sequence, update `progress.md` so the next conversation can pick it up.
- Write down only what you saw through your tools, read in the manuals or worked out in the shell, and what you concluded from it. Mark notes about the machine's current state (position, disks, what is loaded) as state, with when you saw it: the machine may have been restarted since, so check state by looking before relying on it.
- Keep notes short and merged; edit entries rather than adding duplicates. The notebook has size limits.

With the manuals tool present, it adds: The manuals that came with the program are on your shelf (`manuals`). Look things up there before guessing at commands or rules, and when the program asks a question from its manual.

With the shell present (no `--no-shell`), it adds: You have a sandboxed shell (`code_execution`) for working things out: puzzles, routes over your own map notes, arithmetic, decoding what you have seen. It cannot reach the machine. Its files may be lost when its container expires; copy code worth keeping into your notebook's `scripts/`.

The prompt says nothing about the harness, the operator connection or the contents of checkpoints beyond what the tool list shows (D4), and nothing taken from the notebook or the manuals (§5.3).

## 14. Tests

**Unit tests** (no adapter, no API, no terminal; `FakeSession` and a scripted `FakeAnthropic` whose `beta.messages.stream` yields scripted text deltas and a final message):
1. `tools()` maps the name, description and `inputSchema` exactly, in MCP order. An adapter tool named `memory`, `manuals` or `code_execution` stops the client with exit 2.
2. `call()` converts text, image, mixed, empty and error results as in §4.
3. `send` finishes with reason `done` on `end_turn` with no client tool use.
4. `send` runs two `tool_use` blocks in order and sends both results in one user message, in order, with matching ids.
5. Append-only: across a scripted five-turn conversation with two `send` calls, every request's `messages` starts with the previous request's.
6. The request carries `context_management` with the flag values, and leaves it out with `--no-clear`. It also carries `thinking`, `output_config.effort`, `fallbacks` and both betas, and no `tool_choice`, `temperature`, `top_p` or `top_k`. Its `tools` end with the memory, manuals and code execution entries in that order; `--no-memory`, no manuals folder and `--no-shell` each leave theirs out, and the system prompt's matching section with it.
7. `refusal`, `max_tokens` and an unknown stop reason each end `send` with the matching reason and a transcript record.
8. `--max-turns 3` ends a `send` after exactly 3 requests, and the limit applies again to the next `send`.
9. A failure of the MCP session ends the session with exit 2, and `summary.json` is still written. An SDK error ends one `send` with reason `error` and the next `send` works.
10. Transcript: images are written as files and recorded by path and sha256; no record holds base64 data; no record or log holds the value of `ANTHROPIC_API_KEY` (set it to a known dummy in the test).
11. Usage errors (exit 2): `--clear-keep 0`, both `--goal` and `--goal-file`, `--once` without a goal, `--state-dir` after `--`, and an existing `--run-dir`.
12. **Steering.** `steer("x")` during the first tool call: the next request's last user message holds the `tool_result` blocks and then the text `x`, in that order. Steering during the final request, answered by `end_turn`: the loop continues with `x` as a new user message and does not stop.
13. **Interrupt during a request.** The partial response is not in the next request's history; `send` ends `interrupted`; the next `send("y")` gives a request whose history ends with the earlier user message and `y` as a further user message.
14. **Interrupt during a tool call.** A response with three `tool_use` blocks, interrupted while the first runs: the first finishes and keeps its real result, the other two get an error result, all three `tool_result` blocks are in one user message, and `send` ends `interrupted`.
15. `steer` and `interrupt` called when nothing is running are ignored without an error.
16. `turn_done` reports the usage and `applied_edits`; a change of `response.model` between turns produces a `notice`.
17. Dispatch: `memory` goes to the store, `manuals` to the manuals tool, and nothing else reaches either; a `code_execution` server block in a response is recorded and reported and gets no `tool_result`.
18. **`pause_turn`.** A scripted `pause_turn` response is appended and the next request is the same history with nothing added; the turn count includes it.

**Campaign tests:**
19. The default name: `--disk "Ultima IV Disk 1.dsk"` gives `campaigns/ultima-iv-disk-1/`; no `--disk` gives `campaigns/default/`; `--campaign runs/x` (a path) is used as given.
20. The adapter is started with `--state-dir <campaign>/machine`, plus `--fresh-machine` and `--fresh-disks` when given.
21. A second client on a locked campaign exits 2 naming it.
22. The startup note: for each `startup.json` case of §7.3 (restored, each fresh reason, `--fresh-disks`, first session) the first user message starts with the matching line and a notice is shown; `/new` adds none.
23. `--manuals DIR` is stored in `campaign.json` and used by a later session without the flag.

**Memory tests** (`MemoryStore` on a temporary directory):
24. Each command returns the strings in the memory tool docs: `view` of the root on an empty store, of a directory (two levels, hidden items left out, tab between size and path), of a file (6-wide right-aligned line numbers, a tab, 1-indexed) and with `view_range` (`[2, 3]` and `[2, -1]`); `create`; `str_replace` (success, not found, several occurrences with their line numbers, `new_str` omitted deletes); `insert` (line 0, last line, out of range); `delete` (file, directory recursively); `rename` (success, destination exists). A missing path gives each command's documented error.
25. `create` on an existing file is refused and leaves the file unchanged. `delete` and `rename` of `/memories` are refused.
26. Each of these paths is refused and nothing outside the root is created, read or changed: `/memories/../x`, `/memories/%2e%2e/x`, `/memories/a/../../x`, `/memories\..\x`, `C:/x`, `/etc/passwd`, `/memoriesx/a`, `/memories/.client-state.json`, `/memories/.hidden/a`, and a symlink inside the root that points outside it (skipped where the platform cannot make one).
27. Limits: a `create` or edit that makes a file pass 64 KiB, the store pass 1 MiB, or the store pass 200 files is refused with the limit named, and nothing is written. A `view` of a 20,000-character file returns 16,000 characters and the truncation line.
28. Atomic writes: with `os.replace` patched to fail, a `str_replace` leaves the file exactly as before and no temporary file behind.
29. Outside edits: end a session (the state file is written), change a note, start again: `memory_edited_outside` is true in `run.json` and a notice is shown. With no change it is false. With no state file it is false.

**Shell tests** (scripted `FakeAnthropic` and a fake Files API):
30. The first response's container id is stored in `shell.json` and sent as `container` on every later request, including in the next session.
31. A "container expired" error drops the id, retries without `container`, shows a notice, and adds the harness line to the next user message; the run goes on.
32. A new container gets one `container_upload` block per manual; an unchanged manual is not uploaded again (same `file_id`), a changed one is. No disk image, notebook file or save state is ever uploaded (the fake Files API records every upload).

**Manuals tests** (a temporary folder with a text file, a PDF with a text layer, a PDF of scanned pages without one, a PNG):
33. `list` shows each file with its kind and size, page counts and text-layer flags for the PDFs, and an unsupported file as such; hidden files are not listed.
34. `search` finds words in the text file and the text-layer PDF with page and line numbers, caps at 20 hits, and names the scanned PDF as having no text layer.
35. `read` returns numbered lines of the text file; `pages: "2-3"` of a PDF returns a `document` block whose PDF has exactly those two pages; the PNG returns an image block; 6 pages, 401 lines, a range past the end, an unknown file and an unsupported kind are errors.
36. Confinement: `../x`, an absolute path, a drive path, a hidden segment and a symlink out of the folder are refused, and nothing outside it is read.

**Chat tests** (`prompt_toolkit`'s `create_pipe_input` and `DummyOutput`, `Conversation` replaced by a scripted fake):
37. A line typed at an idle prompt calls `send`; a line typed while `running` calls `steer` and is echoed as queued; Esc and Ctrl+C while running call `interrupt`; Ctrl+C twice quits.
38. `/help`, `/usage`, `/effort`, `/new`, `/show`, `/memory`, `/campaign` and `/quit` do what §11 says; an unknown command sends nothing; `/new` while running is refused with a message. `/memory` lists open hypotheses from a `hypotheses.md` fixture.
39. An empty line sends nothing. A message with a newline (Alt+Enter) is sent whole.
40. Event rendering: a tool call is one line, an image result shows the file path, a memory or manuals call is one line with no result line, a shell call shows its command and first output line, a long input is truncated to the width.

**Integration test** (real adapter and harness, scripted model, no API cost; needs the persistence spec implemented). `FakeAnthropic` scripts four turns: `memory view /memories`, `look`, then `type_text` with `"PRINT 2+2\n"` together with `memory create /memories/facts.md`, then `end_turn`. Run it with `--once` against the adapter with `--headless --disk` on the DOS 3.3 System Master (the fixture the adapter tests use), with `--campaign` in a temporary directory. Assert:
- the second tool result holds an image block with `media_type` `image/png`, and `images/2-1.png` decodes as a PNG;
- `facts.md` exists in the notebook with the scripted text;
- the run exits 0, and `machine/last_session.json` says the stepped session ended cleanly with an end checkpoint.

A second `--once` run on the same campaign, scripted to `view /memories/facts.md` and `look`, gets that text back and starts with the "restored" startup note: the notebook and the machine survived the process. A third run with `--realtime` starts with a "started fresh" note. A fourth scripted conversation drives two `send` calls through one `Conversation` on the same adapter and checks that the second sees the emulator state the first left.

Skipped when `HARNESS_EXE` is not set, like the adapter tests.

**Live smoke test** (real API, costs money). It runs only when `APPLEWIN_LIVE_SMOKE=1`, and only after the user has approved the spend. With a fresh campaign and a manuals folder holding one short text file, goal: "Type PRINT 2+2 and tell me what the computer answered. Then use the shell to compute 2**40 and tell me." Run it with `--once --clear-trigger 3000 --clear-keep 2 --max-turns 20`. Check:
- the run exits 0 and the final text contains `4` and `1099511627776`;
- the agent viewed `/memories` before its first adapter call, and wrote at least one note;
- a container id was stored, and the code execution result is in the transcript;
- at least one `applied_edits` entry appears;
- no request fails because of the clearing.

Then, in one `Conversation` on the same campaign, send a second goal ("Now print the result of 3*7."), and interrupt a third message while its request is in flight; check that the following `send` is accepted by the API (consecutive user messages, §5.4) and that preserved thinking raises no error after the dropped response. A last request asks the agent to `read` page 1 of a small PDF from the manuals, checking that a `document` block in a `tool_result` is accepted (§15 point 12).

This is where open points 1, 2, 3, 12 and 13 in §15 are checked, and where point 10 gets its first evidence.

## 15. Open points for review

1. **Clearing and preserved thinking.** The context-editing docs don't say whether clearing tool results interacts with Opus 5.5's check that the history was not edited. The client stays append-only, so it should be fine; the live smoke test confirms it. If it fails, the fallback is `--no-clear` plus a shorter conversation.
2. **Images in cleared results.** The docs don't say whether clearing removes image blocks inside a `tool_result`. The smoke test compares `cleared_input_tokens` with the images' visual-token cost to tell.
3. **Dropped responses and consecutive user messages.** §5.4 relies on the API combining consecutive user turns and on preserved thinking accepting a history from which an incomplete response was never appended. The live smoke test checks both; the fallback for the first is stated in §5.4.
4. **Defaults.** `--clear-trigger 40000`, `--clear-keep 6` and `--clear-at-least 10000` are first guesses: a screenshot is 280 tokens, and the tool list is several thousand. Tune them from measured runs (A/B against `--no-clear`).
5. **Model.** The default is `claude-opus-5-5`. A cheaper model for long play sessions is a `--model` choice, not a code change.
6. **Forgetting the last image.** An adapter call that forgets the last image would make the "same as the last image" note always true. The client would make it when `context_management.applied_edits` reports cleared tool uses. It is a new tool on the adapter, so the user decides whether to add it.
7. **Compaction.** A chat that lasts hours outgrows the context window even with screenshots cleared. The API's compaction on demand (beta `compact-2026-09-04`, top-level `compaction` parameter) fits a chat, because the client can compact at an idle prompt or a turn boundary, and the notebook keeps what a summary might drop. Its parameters and the conditions under which kept turns keep their thinking (the "Compaction and preserved thinking" page) must be read from the live docs before it is designed. v1 does without (§5.7).
8. **Resuming a closed session's conversation.** Not in v1. It needs the raw message history, including thinking blocks and their signatures, which the transcript deliberately does not keep (§10). The campaign covers most of the need: a new session restores the machine where it can, reads the notebook and continues.
9. **Human messages in play runs.** Steering, interrupts and notebook edits let a person nudge the agent. That is the intent, and the transcript and `summary.json` mark every one, so no run with guidance passes for an unaided one. Unaided runs use `--once --no-memory`, or `--once` with a fresh campaign.
10. **Notebook quality.** Whether the agent keeps the layout, tests hypotheses cheapest first and keeps notes short is a prompt question, settled only by real sessions. Read the notebooks after the first long sessions and adjust §13; the limits in §6.1 are first guesses too.
11. **Campaign scope.** One campaign per disk is the default. A program on several disks (a boot disk and a scenario disk) gets its campaign from the boot disk given as `--disk`; the other disks the agent inserts are saved in the same campaign (persistence spec §4).
12. **Code execution details.** The tool version (`code_execution_20260521`), whether it needs a beta header, the exact `container` request field and response shape, the expiry error, where `container_upload` files land in the container, and whether context editing clears server tool results must be read from the live docs before implementation, and are checked by the smoke test.
13. **PDF pages in tool results.** §9 returns PDF pages as a `document` block inside a `tool_result`. If the API does not accept that, the fallback is to return a text line in the `tool_result` and put the `document` block in the same user message after the tool results (where steering text goes), which the API accepts.
14. **Programmatic tool calling** (code in the container calling the adapter's tools) is not used: every adapter call goes through the agent, one at a time, and is recorded. Whether scripted input from the shell would be fair is a question for the user if it is ever proposed.
15. **The review of 2026-11-02** (`bg3-agent-harness/research`) also proposed: an automatic fresh start with a handoff at a context size, a history and deletion guard for the notebook, provenance tags including `[recalled]` for knowledge from training, a stuck detector, screenshots kept in the notebook, forced reflections at goal ends, and a "read when" index with hypothesis expiry. They are not in this revision; they need no new power over the machine.

## 16. Done when
- Every test in §14 except the live smoke passes on CI (`python -m unittest discover -s tests/client -v`). The integration test needs `docs/specs/persistence.md` implemented first.
- `client/README.md` shows one complete command line for each of the interactive chat and `--once`, lists the slash commands and keys, and explains the session directory and the campaign (what carries over in each run mode, the notebook's layout, that the person may edit it and that edits are marked, the manuals folder, the shell and its container).
- `THIRD-PARTY-NOTICES.txt` lists `anthropic`, `prompt_toolkit` and `pypdf`; `.gitignore` lists `runs/` and `campaigns/`.
- The live smoke test has been run once with the user's approval, and its `summary.json` and the notebook it left are attached to the PR.
