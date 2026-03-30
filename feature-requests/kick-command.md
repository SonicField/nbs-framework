# Feature Request: /kick Command

## Problem

When a single agent goes wonky (stuck, confused, producing nonsense), the human must either wait for fixup's next cycle or manually kill the session via `nbs-ts kill`. Neither is good — fixup may be 30 minutes away, and manual `nbs-ts kill` doesn't reset the cursor, respawn, or verify.

## Proposal

A `/kick <agent>` command in `nbs-chat-terminal` that performs a Level 4 hard restart on a single named agent, immediately, while leaving all other agents running.

```
alex> /kick scribe
  INFO> [kick] Killing scribe session...
  INFO> [kick] Cursor reset to 428
  INFO> [kick] Respawning scribe...
  INFO> [kick] scribe alive and verified
alex>
```

## What It Does

Exactly fixup's Level 4 sequence for one agent:

1. Find the alive session `nbs-<agent>-<tag>`
2. Kill it (`nbs-ts kill`)
3. Clean up PID file
4. Reset cursor to current message count
5. Spawn via `launch_agent`
6. Wait 30 seconds
7. Verify session is alive
8. Report result via INFO lines

## Implementation

In `terminal.c`, after the existing slash command handlers:

```c
if (strncmp(edit.buf, "/kick ", 6) == 0) {
    const char *agent = edit.buf + 6;
    // validate agent name against expected list
    // derive tag from chat file
    // spawn_with_capture a script that does the Level 4 sequence
}
```

The Level 4 logic lives in a bash script (`bin/nbs-kick-agent`) rather than inline C — it needs `nbs-ts`, `launch_agent`, `sed`, and cursor file manipulation. The terminal captures its output via `spawn_with_capture` and streams as INFO lines.

## Validation

The agent name must be one of: scribe, medic, supervisor, gatekeeper, theologian, testkeeper, generalist. Reject anything else with an INFO error.

## Files

| File | Change |
|------|--------|
| `bin/nbs-kick-agent` | New — bash script performing Level 4 on one agent |
| `src/nbs-chat/terminal.c` | Add `/kick` command handler |
| `docs/tools/nbs-chat-terminal.md` | Document `/kick` |

## What Does NOT Change

- Fixup — still runs on schedule, still checks all agents
- Other agents — untouched by `/kick`
- The watchdog — not involved
