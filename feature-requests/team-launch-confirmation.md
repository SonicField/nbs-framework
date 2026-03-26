# Feature Request: Team Launch Confirmation

## Problem

`nbs-chat-terminal-restart.sh` launches 7 agents and prints "Team restarted successfully" without verifying anything actually started. If the helper is down, a session fails to create, or a sidecar dies, nobody knows until the human notices silence in chat.

New users hit this — agents show "Done" immediately, no error message, no guidance.

## Proposal

A standalone script `bin/nbs-team-check` that verifies team health after launch. Called by the restart script but also usable independently for debugging.

### Usage

```bash
nbs-team-check <chat-tag> <project-root>
```

### What It Checks

1. **nbs-ts-helper running** — `pgrep nbs-ts-helper`. Without it, nothing works.
2. **Expected sessions alive** — `nbs-ts list --name=<tag>` for each of the 7 agents (scribe, medic, supervisor, gatekeeper, theologian, testkeeper, generalist). Each should have an alive session.
3. **Sidecars running** — `pgrep -f "nbs-sidecar.*--handle=<agent>"` for each agent. Session without sidecar means no notifications.
4. **Session responding** — optionally, `nbs-ts read-new <handle> --strip` to check the session has produced output (Claude started).

### Output: All Good

```
[team-check] All 7 agents alive, 7 sidecars running. Team healthy.
```

### Output: Problems Found

```
[team-check] POST-LAUNCH CHECK FAILED

  scribe:      session alive, sidecar running — OK
  medic:       session alive, sidecar running — OK
  supervisor:  session alive, sidecar running — OK
  gatekeeper:  NO SESSION FOUND
  theologian:  session alive, NO SIDECAR
  testkeeper:  session alive, sidecar running — OK
  generalist:  NO SESSION FOUND

  2 agents failed to start. 1 sidecar missing.

  Troubleshooting:
    - Is nbs-ts-helper running?  pgrep nbs-ts-helper
    - Check debug logs:          ls /tmp/nbs-claude-debug-*.log
    - Check helper log:          (wherever you started nbs-ts-helper)
    - Manual test launch:
        NBS_DEBUG=1 NBS_HANDLE=test NBS_TRANSPORT=ts \
          NBS_INITIAL_PROMPT="say hello" \
          .nbs/bin/nbs-claude --root=$(pwd) --dangerously-skip-permissions
```

### Integration with Restart Script

In `nbs-chat-terminal-restart.sh`, after the spawn loop:

```bash
sleep 30
"${NBS_BIN}/nbs-team-check" "$CHAT_TAG" "$PROJECT_ROOT"
```

The 30-second wait gives agents time to initialise their nbs-ts sessions and sidecars.

### Exit Codes

- 0 — all agents and sidecars healthy
- 1 — one or more agents or sidecars missing
- 4 — invalid arguments

### Implementation Notes

- Pure bash — no C needed
- Reuses `nbs-ts list`, `nbs-ts status`, `pgrep`
- Should be fast (<2 seconds for all checks)
- Does NOT attempt to fix problems — only reports them
- Standalone so users can run it any time: `nbs-team-check poem /data/users/me/myproject`
