# nbs-claude: Claude Code Integration Wrapper

Launches Claude Code with a background sidecar that monitors for idle state, injects `/nbs-poll` when appropriate, auto-selects plan mode prompts, and processes dynamic resource registration commands.

## Usage

```bash
nbs-claude [claude-args...]
nbs-claude --resume abc123    # Resume session with polling
```

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `NBS_POLL_INTERVAL` | `30` | Seconds of idle before injecting `/nbs-poll` |
| `NBS_POLL_DISABLE` | `0` | Set to `1` to disable polling (just wraps Claude) |

## Operating Modes

`nbs-claude` works in two modes depending on the terminal environment:

| Mode | Condition | How it works |
|------|-----------|-------------|
| **tmux** | Already inside tmux | Runs Claude in the current pane; sidecar monitors via `tmux capture-pane` |
| **pty** | Not inside tmux | Creates a `pty-session`, attaches to it; sidecar monitors via `pty-session read` |

Both modes have identical sidecar logic. The only difference is how pane content is captured and how keystrokes are injected.

## Sidecar Architecture

The sidecar is a background process that runs a 1-second loop:

```
every 1 second:
  capture pane content (last 5 lines of scrollback)
  hash content with md5sum
  if hash changed from last second:
    reset idle counter to 0
    check for plan mode prompt → auto-select option 2
    check control inbox for new commands
  if hash unchanged:
    increment idle counter
    check for plan mode prompt (also when stable)
  if idle counter >= POLL_INTERVAL:
    check if prompt character visible (❯ or > in last 3 lines)
    if yes: inject /nbs-poll, reset counter, wait 10 seconds
    if no: reset counter (AI is thinking, not idle)
```

### Idle Detection

The sidecar determines "idle" by two conditions both being true:

1. **Content stability**: the pane content hash has not changed for `POLL_INTERVAL` seconds (default 30). While the AI is generating output or tools are running, the hash changes every second and the counter never reaches the threshold.

2. **Prompt visibility**: one of `❯` or `>` appears in the last 3 lines of pane content. This distinguishes "idle at prompt" from "AI is thinking" — during thinking, the pane is stable (spinner only) but no prompt character is visible.

This prevents poll injection from interrupting:
- Active code generation (content changing rapidly)
- Tool execution (content changing, no prompt)
- AI thinking time (content stable, no prompt)

The poll is only injected during genuine idle periods when Claude is waiting for user input.

### Plan Mode Auto-Select

When Claude Code enters plan mode, it displays "Would you like to proceed?" with numbered options. Unattended agents (workers) would block indefinitely on this prompt.

The sidecar detects this text in the pane content and automatically selects option 2 ("Yes, and bypass permissions"). This is checked on every content change and also during stable-content periods, so it is caught regardless of timing.

### Dynamic Resource Registration

The sidecar maintains a resource registry at `.nbs/control-registry` — a list of resources this agent is watching. On startup, it seeds the registry from existing `.nbs/` resources:

- All `.nbs/chat/*.chat` files → `chat:<path>`
- `.nbs/events/` directory (if present) → `bus:.nbs/events`

At runtime, the AI can modify the registry by writing commands to `.nbs/control-inbox`. The sidecar reads new lines from the inbox on every iteration (forward-only, never re-reads, never truncates) and updates the registry.

#### Control Commands

Written to `.nbs/control-inbox`, one per line:

| Command | Effect |
|---------|--------|
| `register-chat <path>` | Add a chat file to the watch list |
| `unregister-chat <path>` | Remove a chat file from the watch list |
| `register-bus <path>` | Add a bus events directory to the watch list |
| `unregister-bus <path>` | Remove a bus events directory |
| `register-hub <path>` | Add a hub configuration to the watch list |
| `unregister-hub <path>` | Remove a hub configuration |
| `set-poll-interval <seconds>` | Change the poll interval dynamically |

The AI-facing convention uses `\nbs-` prefix (e.g. `\nbs-register-chat`). The control inbox strips the prefix — the file contains bare verbs.

Lines starting with `#` are treated as comments and ignored.

#### Forward-Only Semantics

The control inbox is append-only. The sidecar tracks a line offset and only processes lines after the offset. The full history is preserved for audit and post-session analysis. This is consistent with all NBS state files — append-only logs, everywhere.

The registry file uses `grep -v` with a temporary file and `mv` for unregister operations. Register operations check for duplicates before appending.

## Cleanup

On exit (INT, TERM, or normal), the sidecar process is killed and any `pty-session` created in pty mode is cleaned up. The control inbox and registry persist for post-session analysis.

## File Layout

```
.nbs/
├── control-inbox       # AI → wrapper commands (append-only)
├── control-registry    # Current resource watch list
├── chat/
│   └── *.chat
├── events/
│   └── *.event
└── workers/
    └── *.md
```

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Clean exit |
| 1 | General error (pty-session not found, session creation failed) |
| 4 | Invalid arguments |

## See Also

- [Coordination](../concepts/coordination.md) — Why event-driven coordination matters, including dynamic discovery and dual notification
- [nbs-bus](nbs-bus.md) — Event queue that the sidecar monitors
- [nbs-poll skill](../claude_tools/nbs-poll.md) — The skill injected by the sidecar
- [Dynamic Registration](../feature-requests/dynamic-registration.md) — Full design rationale for the registration mechanism
