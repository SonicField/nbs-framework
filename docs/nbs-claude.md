# nbs-claude: Claude Code Integration Wrapper

Launches Claude Code with a bus-aware background sidecar that checks for pending events and unread chat messages, injecting `/nbs-notify` when there is something to process. Falls back to `/nbs-poll` as a safety net after extended idle. Auto-selects plan mode prompts and processes dynamic resource registration commands.

## Usage

```bash
nbs-claude [claude-args...]
nbs-claude --resume abc123    # Resume session with polling
```

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `NBS_POLL_INTERVAL` | `300` | Safety net: seconds of idle before injecting `/nbs-poll` |
| `NBS_POLL_DISABLE` | `0` | Set to `1` to disable all polling (just wraps Claude) |
| `NBS_BUS_CHECK_INTERVAL` | `3` | Seconds between bus/chat checks |
| `NBS_NOTIFY_COOLDOWN` | `15` | Minimum seconds between `/nbs-notify` injections |
| `NBS_HANDLE` | `claude` | Agent handle for cursor peeking |
| `NBS_ROOT` | `.` | Project root containing `.nbs/` (resolved to absolute path at startup) |
| `NBS_STARTUP_GRACE` | `30` | Seconds after init before allowing notifications |
| `NBS_INITIAL_PROMPT` | *(none)* | Custom initial prompt sent on startup (default: handle + `/nbs-teams-chat`) |
| `NBS_NOTIFY_FAIL_THRESHOLD` | `5` | Consecutive failed `/nbs-notify` injections before self-healing activates |
| `NBS_STANDUP_INTERVAL` | `15` | Minutes between standup check-in messages posted to chat (0 to disable) |

## Operating Modes

`nbs-claude` works in two modes depending on the terminal environment:

| Mode | Condition | How it works |
|------|-----------|-------------|
| **tmux** | Already inside tmux | Runs Claude in the current pane; sidecar monitors via `tmux capture-pane` |
| **pty** | Not inside tmux | Creates a `pty-session`, attaches to it; sidecar monitors via `pty-session read` |

Both modes have identical sidecar logic. The only difference is how pane content is captured and how keystrokes are injected.

## Sidecar Architecture

The sidecar is a background process that runs a 1-second loop with two notification tracks:

```
every 1 second:
  check control inbox for new commands
  capture pane content (last 5 lines of scrollback)
  hash content with md5sum
  if hash changed from last second:
    reset idle and bus-check counters
    check for plan mode prompt → auto-select option 2
  if hash unchanged:
    increment idle counter and bus-check counter
    check for plan mode prompt (also when stable)

  # Track 1: Bus-aware fast check (every BUS_CHECK_INTERVAL seconds)
  if bus_check_counter >= BUS_CHECK_INTERVAL:
    reset bus_check_counter
    if prompt visible AND (bus events pending OR chat unread):
      inject /nbs-notify <summary>
      wait 10 seconds

  # Track 2: Safety net (every POLL_INTERVAL seconds)
  if idle_seconds >= POLL_INTERVAL:
    if prompt visible:
      inject /nbs-poll
      wait 10 seconds
```

### Bus-Aware Notification

The sidecar checks the event bus and chat cursors directly, injecting `/nbs-notify` only when there is something to process. This eliminates the context waste of blind polling.

**Bus checking** (`check_bus_events`): Reads the control registry for `bus:` entries, runs `nbs-bus check` (non-destructive) on each. If pending events exist, notes the count and highest priority.

**Chat cursor peeking** (`check_chat_unread`): For each registered chat, reads the cursor file (`<chat>.cursors`) directly — no lock acquired, no cursor advanced. Compares the handle's cursor position against the total message count. The AI's cursor is never modified by the sidecar; it is only advanced when the AI reads messages via `nbs-chat read --unread`.

**Cooldown**: After injecting `/nbs-notify`, the sidecar waits at least `NBS_NOTIFY_COOLDOWN` seconds (default 15) before injecting again. Critical-priority bus events bypass the cooldown.

**Summary message**: The sidecar passes a summary as the `/nbs-notify` argument, e.g.: `2 event(s) in .nbs/events/. 3 unread in live.chat`. This is capped at 200 characters for tmux safety.

### Idle Detection

The sidecar determines "idle" by two conditions both being true:

1. **Content stability**: the pane content hash has not changed for the required interval. While the AI is generating output or tools are running, the hash changes every second and the counter never reaches the threshold.

2. **Prompt visibility**: one of `❯` or `>` appears in the last 3 lines of pane content. This distinguishes "idle at prompt" from "AI is thinking" — during thinking, the pane is stable (spinner only) but no prompt character is visible.

This prevents injection from interrupting:
- Active code generation (content changing rapidly)
- Tool execution (content changing, no prompt)
- AI thinking time (content stable, no prompt)

### Plan Mode Auto-Select

When Claude Code enters plan mode, it displays "Would you like to proceed?" with numbered options. Unattended agents (workers) would block indefinitely on this prompt.

The sidecar detects this text in the pane content and automatically selects option 2 ("Yes, and bypass permissions"). This is checked on every content change and also during stable-content periods, so it is caught regardless of timing.

### Self-Healing After Skill Loss

When Claude Code compacts context, it can lose its registered skills. The sidecar detects this by checking for "Unknown skill" in pane content after injecting `/nbs-notify`. If the skill is rejected, the sidecar increments a failure counter (`NOTIFY_FAIL_COUNT`).

After `NBS_NOTIFY_FAIL_THRESHOLD` consecutive failures (default 5), the sidecar switches to a recovery mode: instead of injecting `/nbs-notify`, it sends a raw text prompt containing:

- Absolute paths to the skill files (`nbs-notify.md`, `nbs-teams-chat.md`, `nbs-poll.md`) resolved via `realpath`
- The agent's handle
- Instructions to announce recovery on the first registered chat file

This bypasses the skill registration system entirely, asking the agent to read the skill files directly. If the recovery succeeds (no "Unknown skill" in subsequent output), the failure counter resets to 0 and the sidecar resumes normal `/nbs-notify` injection.

The detection function (`detect_skill_failure`) matches the exact error string produced by Claude Code when a skill symlink is dangling or a skill name is unregistered.

### Deterministic Pythia Trigger

The sidecar includes a deterministic checkpoint trigger for Pythia trajectory assessments. Rather than relying on AI judgement to decide when an assessment is due, the sidecar counts `decision-logged` events in the bus processed directory (`.nbs/events/processed/`) and publishes a `pythia-checkpoint` event when the count crosses a multiple of the configured interval.

**Configuration**: The interval is read from `.nbs/events/config.yaml`:

```yaml
pythia-interval: 20
```

The default is 20 if no config file exists or the value is missing. The value must be a positive integer.

**Behaviour**: On each `should_inject_notify` cycle, `check_pythia_trigger` runs independently of the notify decision. It uses bucket arithmetic (`decision_count / interval`) to detect threshold crossings. This prevents re-triggering at the same count and handles catch-up correctly on first run — if 40 events already exist when the sidecar starts, it triggers once and syncs its counter.

**State**: The trigger count is tracked in-memory via `PYTHIA_LAST_TRIGGER_COUNT` (not persisted across restarts). This is intentional: on restart, the sidecar catches up to the current count and triggers if a new threshold has been crossed since the last run.

### Deterministic Standup Check-In

The sidecar posts periodic team check-in messages directly to chat, prompting all agents to report their status and look for useful work. This prevents the "everyone is idle because no one has anything to say" stall.

**Configuration**:

| Variable | Default | Description |
|----------|---------|-------------|
| `NBS_STANDUP_INTERVAL` | `15` | Minutes between standup messages (0 to disable) |

**Behaviour**: After `NBS_STANDUP_INTERVAL` minutes of wall-clock time since the last standup, the sidecar posts to the first registered chat: `@all Check-in: what are you working on? What is blocked? What could we be doing? If idle, find useful work.`

This message appears as a normal chat message from `sidecar`, triggering every agent's unread detection and causing them to respond with status updates. Duplicate prevention uses CSMA/CD (Carrier Sense Multiple Access with Collision Detection): before posting, each sidecar checks the last 5 chat messages for a recent standup. If one exists, it backs off and resets its timer. Random jitter (±2 minutes) on the interval reduces the probability of simultaneous posts to near zero.

**First run**: On startup, the timer is initialised without posting. The first standup fires after the full interval elapses.

The sidecar maintains a per-agent resource registry at `.nbs/control-registry-<handle>` — a list of resources this agent is watching. On startup, it seeds the registry from existing `.nbs/` resources:

- All `.nbs/chat/*.chat` files → `chat:<path>`
- `.nbs/events/` directory (if present) → `bus:.nbs/events`

At runtime, the AI can modify the registry by writing commands to `.nbs/control-inbox-<handle>`. The sidecar reads new lines from the inbox on every iteration (forward-only, never re-reads, never truncates) and updates the registry.

#### Control Commands

Written to `.nbs/control-inbox-<handle>`, one per line:

| Command | Effect |
|---------|--------|
| `register-chat <path>` | Add a chat file to the watch list |
| `unregister-chat <path>` | Remove a chat file from the watch list |
| `register-bus <path>` | Add a bus events directory to the watch list |
| `unregister-bus <path>` | Remove a bus events directory |
| `register-hub <path>` | Add a hub configuration to the watch list |
| `unregister-hub <path>` | Remove a hub configuration |
| `set-poll-interval <seconds>` | Change the safety net poll interval dynamically |

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
├── control-inbox-<handle>   # AI → wrapper commands (append-only, per-agent)
├── control-registry-<handle> # Current resource watch list (per-agent)
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
- [nbs-notify skill](../claude_tools/nbs-notify.md) — Lightweight notification skill injected when events are pending
- [nbs-poll skill](../claude_tools/nbs-poll.md) — Safety net skill injected after extended idle
- [Dynamic Registration](../feature-requests/dynamic-registration.md) — Full design rationale for the registration mechanism
