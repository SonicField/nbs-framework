# Feature Request: Terminal Watchdog Daemon

**Date:** 2026-03-01
**Status:** Ready to implement
**Priority:** High — team self-terminates when human is away

## Problem

The team declares "session complete" and shuts itself down when the human steps away. No persistent process survives to restart them. The human returns from dinner to find all agents dead and work stopped.

Current architecture gap: sidecars die with their agents (SIGHUP, now partially fixed). Fixup is spawned by sidecars — if all sidecars are dead, no fixup runs. Nothing restarts a fully dead team.

## Solution

A background daemon thread in `nbs-chat-terminal` that detects total team death and performs a Level 4 cold restart with a continuation directive. The terminal is the natural watchdog — it runs as long as the human is nominally present, and stopping it is an explicit "I'm done" signal.

## Design

### Daemon Thread

- Spawned on terminal startup as a `pthread` background thread
- Polls `tmux list-sessions | grep nbs-.*-live` every 60 seconds
- Counts active agent sessions (scribe, gatekeeper, testkeeper, theologian, generalist, supervisor)
- If fewer than 3 agent sessions alive: trigger restart sequence
- Thread exits when terminal exits (no cleanup needed — it's a daemon)

### Death Detection

```
Every 60 seconds:
  count = number of nbs-*-live tmux sessions
  if count >= 3:
    reset restart counter
    continue
  if restarts_this_hour >= 5:
    post to terminal: "Watchdog: 5 restarts in the last hour. Giving up. Use /restart to manually restart."
    disable watchdog
    continue
  trigger restart
  restarts_this_hour++
```

Threshold of 3 means: if half the team is dead, restart everything. Don't try to patch a partial team — cold restart is simpler and more reliable.

### Restart Sequence

Same as `/nbs-teams-restart` Level 4:

1. Kill all remaining `nbs-*-live` tmux sessions
2. Kill all `nbs-sidecar` processes
3. Clean PID files (`.nbs/pids/*.pid`)
4. Run `nbs-digest-spawn .nbs/chat/live.chat --wait` (blocks until digest posted)
5. Reset all chat cursors to current end
6. Spawn all 6 agents in recovery order (scribe first) with 5-second stagger
7. Inject skills into each agent
8. Post continuation directive to chat

### Continuation Directive

Posted to chat as `supervisor` handle after all agents are up:

```
@team Auto-restart by terminal watchdog. Read the scribe log and chat
digest above. @supervisor create a 6-phase plan to continue and expand
the work from the previous session:

Phase 1-3: Implement the open items from the previous session's fix plan,
benchmarking after each.
Phase 4: Assess against the target. If met, commit and push.
Phase 5: If not met, propose 3 new ideas with falsifiable predictions.
Phase 6: Implement and benchmark each new idea sequentially.

The session is NOT complete until either the target is met or all 6 phases
are exhausted. Diagnosis without implementation is not progress.
```

### `/shutdown` Command

New terminal command. When the user types `/shutdown`:

1. Post to chat: `@team Good work — time to wrap up. Please commit any uncommitted changes, post a final session summary, and shut down cleanly.`
2. Disable the watchdog daemon (set a flag so the daemon thread stops polling)
3. Display in terminal: `Watchdog disabled. Team will not be auto-restarted.`

The terminal itself stays running (the user may want to read chat). The agents receive the shutdown message and can wrap up gracefully. When the last agent exits, the watchdog doesn't restart them.

### `/restart` Command

Manual trigger for the restart sequence. Same as the watchdog's automatic restart but initiated by the user. Useful after the 5-restart limit is hit, or when the user wants to force a fresh start.

### Rate Limiting

- Maximum 5 auto-restarts per rolling hour
- After 5, the watchdog disables itself and posts a warning to the terminal
- `/restart` bypasses the rate limit (it's a manual action)
- Rate counter resets when the team has been alive for 60+ consecutive minutes

### State

Minimal state, all in memory (no files):

```c
typedef struct {
    int enabled;              /* 0 = disabled by /shutdown or rate limit */
    int restart_count;        /* restarts in current hour */
    time_t hour_start;        /* start of current rate-limit window */
    time_t last_check;        /* last poll timestamp */
    time_t last_restart;      /* when last restart completed */
    char chat_path[4096];     /* path to live.chat */
    char project_root[4096];  /* project root for spawning */
} watchdog_state_t;
```

### Thread Safety

The daemon thread only reads tmux state (via `popen`) and writes to chat (via `nbs-chat send`). It doesn't share mutable state with the terminal's main thread except `enabled` (set by `/shutdown`). Use `_Atomic int` for `enabled` to avoid races.

### What the Daemon Does NOT Do

- **Individual agent recovery** — that's fixup's job. The daemon only handles total team death.
- **Goal setting** — it tells supervisor to continue from where the previous session left off. The specific work plan comes from reading the scribe log.
- **Session management** — it doesn't track what the team is working on. It just counts alive sessions.

## Implementation

### Files to Modify

1. **`src/nbs-chat/terminal.c`** — add `watchdog_state_t`, spawn daemon thread on startup, add `/shutdown` and `/restart` command handlers
2. **`bin/nbs-chat-terminal-restart.sh`** — new shell script containing the restart sequence (called by the daemon thread via `system()`). Keeps the restart logic in shell where it's easier to maintain.

### `bin/nbs-chat-terminal-restart.sh`

```bash
#!/bin/bash
set -euo pipefail

PROJECT_ROOT="$1"
CHAT_FILE="$2"

cd "$PROJECT_ROOT"

# Kill everything
for h in scribe gatekeeper testkeeper theologian generalist supervisor; do
    tmux kill-session -t "nbs-${h}-live" 2>/dev/null || true
done
pkill -f 'nbs-sidecar.*--handle=' 2>/dev/null || true
rm -f .nbs/pids/*.pid 2>/dev/null

sleep 2

# Digest
bash bin/nbs-digest-spawn "$CHAT_FILE" --wait

# Reset cursors
HEADER_LINES=6
MESSAGE_COUNT=$(( $(wc -l < "$CHAT_FILE") - HEADER_LINES ))
for handle in scribe gatekeeper testkeeper supervisor generalist theologian; do
    if grep -q "^${handle}=" "${CHAT_FILE}.cursors" 2>/dev/null; then
        sed -i "s/^${handle}=.*/${handle}=${MESSAGE_COUNT}/" "${CHAT_FILE}.cursors"
    else
        echo "${handle}=${MESSAGE_COUNT}" >> "${CHAT_FILE}.cursors"
    fi
done

# Spawn agents
for h in scribe gatekeeper testkeeper theologian generalist supervisor; do
    tmux new-session -d -s "nbs-${h}-live" -c "$PROJECT_ROOT" \
        "NBS_HANDLE=${h} bin/nbs-claude --dangerously-skip-permissions"
    sleep 5
done

# Wait for init
sleep 15

# Inject skills
tmux send-keys -t nbs-scribe-live "/nbs-scribe" Enter; sleep 1
tmux send-keys -t nbs-gatekeeper-live "/nbs-gatekeeper" Enter; sleep 1
tmux send-keys -t nbs-testkeeper-live "/nbs-testkeeper" Enter; sleep 1
tmux send-keys -t nbs-theologian-live "/nbs-theologian" Enter; sleep 1
tmux send-keys -t nbs-generalist-live "/nbs-teams-chat" Enter; sleep 1
tmux send-keys -t nbs-supervisor-live "/nbs-supervisor" Enter

# Post continuation directive
bin/nbs-chat send "$CHAT_FILE" supervisor "@team Auto-restart by terminal watchdog. Read the scribe log and chat digest above. @supervisor create a 6-phase plan to continue and expand the work from the previous session. Phase 1-3: Implement the open items from the previous session fix plan, benchmarking after each. Phase 4: Assess against the target. If met, commit and push. Phase 5: If not met, propose 3 new ideas with falsifiable predictions. Phase 6: Implement and benchmark each new idea sequentially. The session is NOT complete until either the target is met or all 6 phases are exhausted. Diagnosis without implementation is not progress."
```

### Daemon Thread (C, in terminal.c)

```c
static void *watchdog_thread(void *arg) {
    watchdog_state_t *state = (watchdog_state_t *)arg;

    while (state->enabled) {
        sleep(60);
        if (!state->enabled) break;

        /* Count alive agent sessions */
        FILE *fp = popen("tmux list-sessions -F '#{session_name}' 2>/dev/null | grep -c 'nbs-.*-live'", "r");
        if (!fp) continue;
        int count = 0;
        fscanf(fp, "%d", &count);
        pclose(fp);

        if (count >= 3) {
            /* Team alive — reset rate counter if hour elapsed */
            time_t now = time(NULL);
            if (now - state->hour_start >= 3600) {
                state->restart_count = 0;
                state->hour_start = now;
            }
            continue;
        }

        /* Team dead — check rate limit */
        time_t now = time(NULL);
        if (now - state->hour_start >= 3600) {
            state->restart_count = 0;
            state->hour_start = now;
        }

        if (state->restart_count >= 5) {
            /* Display warning in terminal — implementation depends on
             * how terminal.c handles async messages to the display */
            state->enabled = 0;
            continue;
        }

        /* Restart */
        char cmd[8192];
        snprintf(cmd, sizeof(cmd),
                 "bash %s/bin/nbs-chat-terminal-restart.sh '%s' '%s' &",
                 state->project_root, state->project_root, state->chat_path);
        system(cmd);

        state->restart_count++;
        state->last_restart = now;

        /* Wait for restart to complete before next check */
        sleep(120);
    }

    return NULL;
}
```

### New Terminal Commands

```
/shutdown  — Post wrap-up message, disable watchdog
/restart   — Manual trigger of restart sequence (bypasses rate limit)
```

## Dependencies

- `nbs-digest-spawn` (already exists)
- `nbs-chat send` (already exists)
- `nbs-claude` (already exists, now defaults to opus[1m])
- `pthread` (already available in C environment)

## Falsification

The feature works if:
1. Human starts terminal, starts team, walks away
2. Team self-terminates
3. Within 60 seconds, terminal detects death and restarts team
4. New team reads digest and scribe log, continues work
5. Repeat up to 5 times per hour

The feature fails if:
1. Restart sequence fails (build error, missing binary)
2. New team doesn't read the scribe log (continuation breaks)
3. Rate limiting triggers on legitimate team restarts (threshold too low)
4. `/shutdown` doesn't prevent auto-restart (race condition)
