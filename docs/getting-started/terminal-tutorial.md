# Terminal Operator Tutorial

You have a running team. This tutorial teaches you how to operate the terminal — send messages, find things in the chat history, control agents, spawn oracles, and understand what you see on screen.

Each section builds on the previous ones. Work through them in order the first time.

## 1. The Basics

### The Prompt

When you launch the terminal, you see a prompt with your handle:

```
alex>
```

Type a message and press Enter to send it. Your message appears in the chat immediately, styled with a cream handle and dark grey background strip so you can distinguish your own messages from everyone else's.

```
alex> @team Focus on the parser regression today
```

### Incoming Messages

New messages from other participants appear above your prompt automatically. The terminal polls the chat file every 1.5 seconds. You do not need to do anything — messages arrive while you type.

Each agent gets a consistent colour, so you can visually track who is speaking. The Medic's messages appear in terracotta.

### `/help`

Type `/help` to see all available commands and key bindings. This is the in-terminal reference. If you forget a command, start here.

```
alex> /help
```

### `/exit`

Type `/exit` to leave the chat cleanly. Your agents keep running — you are leaving the terminal, not shutting down the team.

```
alex> /exit
Left chat.
```

You can also press Ctrl-C to exit. If you have typed something in the buffer, it is sent before the terminal closes.

## 2. Finding Things

### `/search <pattern>`

Search the entire chat history for a case-insensitive substring. Results show the message index and full content.

```
alex> /search parser
  [42] test-runner> Found 3 failing tests in parser module
  [67] alex> Both of you focus on parse_int first
  2 match(es)
```

If nothing matches, you see "No matches found."

### `/filter <handle>`

Show only messages from one participant. The terminal immediately redisplays the last 50 matching messages so you have context.

```
alex> /filter pythia
  Filtering: showing only messages from pythia
  pythia> [checkpoint] Team started 14:00. 2 tasks assigned.
  pythia> [checkpoint] 3 agents alive, trajectory nominal
```

While the filter is active, the background poller only shows messages from that handle. Your own messages are always suppressed in the poll display — you see them when you send them.

Type `/filter` with no argument to check the current filter status.

### `/unfilter`

Clear the filter and return to showing all messages. The terminal redisplays the last 20 messages from all participants.

```
alex> /unfilter
  Filter cleared — showing last 20 messages
```

### `/mention <handle>`

Show only messages that `@mention` a specific handle. This uses word-boundary matching — `@alex` matches but `@alexander` does not.

```
alex> /mention supervisor
  Mention filter: showing messages mentioning @supervisor
```

The terminal redisplays the last 50 matching messages. Type `/mention` with no argument to check the current mention filter status.

### `/unmention`

Clear the mention filter. The terminal redisplays the last 20 messages.

```
alex> /unmention
  Mention filter cleared — showing last 20 messages
```

### Combining Filters

`/filter` and `/mention` work independently. You can have both active at the same time — for example, `/filter supervisor` to see only the supervisor's messages, then `/mention testkeeper` to narrow further to messages where the supervisor mentions the testkeeper. Clear each with its own command (`/unfilter`, `/unmention`).

## 3. Team Control

### `/pause` and `/resume`

`/pause` freezes the team in place. Agents keep their context but stop receiving new work. The watchdog is disabled, sidecars skip all work, and a `@team` stop order is broadcast.

```
alex> /pause
  Team paused. Type /resume to continue.
```

`/resume` unfreezes the team. The watchdog is re-enabled, a resume message is broadcast, and agents begin receiving work again.

```
alex> /resume
  Team resumed.
```

`/resume` also recovers from `/shutdown` — shutdown disables the watchdog without creating a pause file, so `/resume` handles both cases.

### `/shutdown`

A hard stop. The terminal broadcasts a 10-second warning to `@team`, then kills all agent sessions, sidecars, and nbs-claude processes for the project.

```
alex> /shutdown
```

After shutdown, the terminal itself stays running. You can use `/resume` to re-enable the watchdog and let it restart the team, or `/restart` to restart manually.

### `/restart`

Manually trigger a full team restart. This runs the restart script and bypasses the watchdog's rate limiting.

```
alex> /restart
```

### `/kick <agent>`

Hard restart a single agent without touching the others. The agent's session is killed, its cursor reset, a new session is spawned, and the respawn is verified.

```
alex> /kick scribe
  INFO> [kick] Killing session nbs-scribe-phoenix...
  INFO> [kick] Cursor reset to 428
  INFO> [kick] Respawning scribe...
  INFO> [kick] Waiting for scribe to start...
  INFO> [kick] scribe alive and verified
```

### `/health`

Report team health. This runs `nbs-team-check` and displays the results as INFO lines above your prompt — it shows which agents and sidecars are alive without spawning an oracle.

```
alex> /health
  INFO> [health] All 7 agents alive, 7 sidecars running
```

Or if something is wrong:

```
alex> /health
  INFO> [health] Agents: 6/7 alive
  INFO> [health]   Dead agents: medic
  INFO> [health] Sidecars: 6/7 running
  INFO> [health]   Missing sidecars: medic
  INFO> [health] Suggest: /fixup to diagnose and restart stalled agents
```

The output appears as `[health]` INFO lines. This is a quick, synchronous check — use it when you want a snapshot of who is running without waiting for a full Shepard assessment.

### `/dashboard`

Live full-screen team dashboard. Shows all 7 agents with status, sidecar health, cursor position, last post, and activity. Refreshes every 2 seconds. Press Enter on any agent to drill into their terminal output via `nbs-ts-render`.

```
alex> /dashboard
╔══════════════╤══════════╤══════════╤══════════╤═══════════╤══════════╗
║ Agent        │ Status   │ Sidecar  │ Cursor   │ Last Post │ Activity ║
╟──────────────┼──────────┼──────────┼──────────┼───────────┼──────────╢
║ ▸supervisor  │ alive    │ OK       │ 45/0     │ recent    │ Idle     ║
║  generalist  │ alive    │ OK       │ 43/2     │ recent    │ make -j8 ║
╚══════════════╧══════════╧══════════╧══════════╧═══════════╧══════════╝
```

Navigation: arrow keys to select, Enter to view agent detail, Escape/q to exit. Dead agents and missing sidecars are highlighted in red. Cursor behind >10 is yellow, >50 is red.

### When to Use Each

| Situation | Command |
|-----------|---------|
| Quick check on who is alive | `/health` |
| One agent is behaving strangely | `/kick <agent>` |
| The whole team needs a fresh start | `/restart` |
| You need to step away and want agents to hold position | `/pause`, then `/resume` when you return |
| You are done for the day | `/shutdown` |

## 4. Oracles

Oracles are ephemeral workers. They spawn, perform a specific assessment, post results to chat, and exit. They do not participate in ongoing conversation or accumulate context — fresh perspective is the point.

Oracles work while the team is paused. A common workflow: `/pause`, then `/digest` or `/pythia` to assess the state, then `/resume`.

### `/pythia`

Trajectory and risk assessment. Pythia reads the last 500 lines of the Scribe's decision log and posts a structured checkpoint covering hidden assumptions, second-order risks, missing validation, and a confidence level.

```
alex> /pythia
```

### `/shepard`

Team effectiveness check. Shepard lists active nbs-ts sessions, captures session output, reads the last 20 chat messages, and posts a brief assessment of whether agents are alive and doing useful work.

```
alex> /shepard
```

### `/librarian`

Institutional memory search. The Librarian reads the last 100 chat messages, searches the Scribe's decision log for answers to questions or blockers, and posts findings with an `@team!` tag.

```
alex> /librarian
```

### `/fixup`

Diagnose and restart stalled agents. Fixup runs diagnostics on all agents, identifies stalled or crashed ones, attempts to restart them, and posts a summary.

```
alex> /fixup
```

### `/digest`

Extract structured learnings from the chat. The digest agent reads the full conversation, produces a summary covering decisions, what worked, what didn't, and continuation goals, then posts the digest to chat.

```
alex> /digest
```

### Oracle Output

Oracle output appears as INFO lines above your prompt. INFO lines are ephemeral — they render inline but are not part of the chat history. The oracle's actual assessment is posted to the chat file, where it appears as a normal message from that oracle's handle.

### Oracle Preconditions

All oracle commands require two things before they will spawn:

1. The watchdog must be enabled (team not paused or shut down)
2. The `control-pause` file must not exist

If either check fails, the command prints an error. Use `/resume` first to clear the pause state.

## 5. The Editor and Multi-line Messages

### `/edit`

Opens your `$EDITOR` (defaults to `vim`) in a temporary file. Write your message, save, and quit. The contents are sent as a single message. If you leave the file empty or the editor exits with an error, nothing is sent.

```
alex> /edit
```

This is useful for longer messages — detailed instructions, multi-paragraph feedback, or anything that does not fit comfortably on one line.

The editor runs with a sanitised environment (only `PATH`, `HOME`, `TERM`, and `LANG` are passed through). The temporary file is created with restrictive permissions and deleted after reading.

### `/redraw`

Clears the screen and repaints the last 50 messages. Use this when the display gets corrupted — which can happen after a terminal resize, scroll artefacts, or stray escape sequences from agent output.

```
alex> /redraw
```

`/redraw` respects both active `/filter` and `/mention`, so filtered messages stay filtered.

## 6. Command-Line Flags

These flags are passed when launching `nbs-chat-terminal`, not typed inside the terminal.

### `--restart`

Launch with an immediate team restart. Kills any running agent sessions and starts fresh. You are prompted for confirmation if agents are currently running.

```bash
nbs-chat-terminal .nbs/chat/c11-interp.chat alex --restart
```

### `--goal-file=PATH`

Inject the contents of a file into the chat before the team starts. The file is posted as your handle. This also disables the watchdog's auto-restart thread — goal-file mode relies on fixup for crash recovery instead.

```bash
nbs-chat-terminal .nbs/chat/c11-interp.chat alex --goal-file=goal.md --restart
```

The goal file is validated strictly: it must be a regular file, non-empty, under 64KB, with no null bytes. If validation fails, the terminal aborts before launching any agents.

### `--no-restart`

Disable the watchdog's auto-restart thread. Agents that crash are not automatically respawned — you must use `/restart` or `/kick` manually. Useful when you want to observe a team without interference.

```bash
nbs-chat-terminal .nbs/chat/c11-interp.chat alex --no-restart
```

### @mention Highlighting

`@<handle>` mentions in chat messages are always rendered with inverse video, making them visually prominent. The prompt is also inverted. Matching uses word boundaries — `@alex` matches but `@alexander` does not.

## 7. Understanding the Display

### Agent Colours

Each agent gets a consistent colour in the dark theme. Your own messages are styled differently — cream handle on a dark grey background strip — so they stand out from agent messages. The Medic's messages use a distinctive terracotta colour.

### INFO Lines

INFO lines appear above your prompt during certain operations: team restarts, oracle spawns, `/kick` output, and watchdog status messages. They are rendered inline but are ephemeral — they are not part of the chat file and disappear when the screen is repainted.

INFO lines are labelled with a tag in square brackets indicating the source:

```
[restart] Team restart complete.
[kick] scribe: session killed, respawning...
[pythia] Spawning pythia oracle...
```

### The Prompt in Context

A typical terminal session looks like this:

```
  supervisor> Spawned parser-7b2c — implement C11 expression parser.
  lexer-a3f1> 42/58 tests pass. Working on string literals.
  [restart] Team restart complete.
alex>
```

Agent messages scroll above. INFO lines appear between the last agent message and your prompt. Your cursor sits at the prompt, ready for input.

### Line Editing

The terminal has a built-in line editor. No readline dependency.

| Key | Action |
|-----|--------|
| Left/Right arrows | Move cursor within the line |
| Home/End | Jump to start/end of line |
| Backspace | Delete character before cursor |
| Delete | Delete character at cursor |
| Up/Down arrows | Browse input history |
| Enter | Send the message |
| Ctrl-C | Exit (sends pending input first) |
| Ctrl-D | Exit (sends pending input if buffer non-empty) |

Input history stores the last 50 sent messages. Duplicate consecutive entries are suppressed. Slash commands are included in history — you can up-arrow to recall `/filter pythia` or `/search parser`.
