# nbs-claude: Claude Code Integration Wrapper

Launches Claude Code inside an nbs-ts session with a background sidecar that monitors for pending events, unread chat messages, and blocking dialogues. Handles session creation, sidecar lifecycle, and cleanup.

## Usage

```bash
nbs-claude [claude-args...]
nbs-claude --resume abc123    # Resume session with sidecar
nbs-claude --debug            # Enable debug logging
```

### Debug Mode

Set `NBS_DEBUG=1` or pass `--debug` to log every lifecycle step to `/tmp/nbs-claude-debug-<PID>.log`.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `NBS_HANDLE` | `claude` | Agent handle for identification and cursor tracking |
| `NBS_ROOT` | `.` | Project root containing `.nbs/` (resolved to absolute path) |
| `NBS_BUS_CHECK_INTERVAL` | `3` | Seconds of idle before checking bus/chat |
| `NBS_NOTIFY_COOLDOWN` | `15` | Minimum seconds between notification injections |
| `NBS_STARTUP_GRACE` | `30` | Seconds after start before allowing notifications |
| `NBS_NOTIFY_FAIL_THRESHOLD` | `5` | Consecutive injection failures before self-healing |
| `NBS_FLUSH_INTERVAL` | `60` | Seconds between bare Enter flushes (0 to disable) |
| `NBS_POLL_INTERVAL` | `0` | Legacy `/nbs-poll` safety net, disabled by default |
| `NBS_INITIAL_PROMPT` | *(none)* | Custom initial prompt (default: handle + `/nbs-teams-chat`) |
| `NBS_LIBRARIAN_INTERVAL` | `15` | Minutes between librarian triggers (0 to disable) |
| `NBS_PYTHIA_INTERVAL` | `30` | Minutes between pythia triggers (0 to disable) |
| `NBS_SHEPARD_INTERVAL` | `20` | Minutes between shepard triggers (0 to disable) |
| `NBS_FIXUP_INTERVAL` | `3600` | Seconds between fixup triggers (0 to disable) |

## How It Works

`nbs-claude` creates an nbs-ts session and runs Claude within it. The nbs-ts-helper daemon allocates a PTY, forks, and execs bash which runs Claude — the caller's environment does not reach Claude (it gets the helper's login environment instead). A C sidecar binary (`nbs-sidecar`) is launched via `setsid` to survive shell exits.

The sidecar is documented separately — see [nbs-sidecar](nbs-sidecar.md).

## Resource Registry

The sidecar maintains a per-agent resource registry at `.nbs/control-registry-<handle>`. On startup, it seeds from existing `.nbs/` resources:

- All `.nbs/chat/*.chat` files as `chat:<path>`
- `.nbs/events/` directory (if present) as `bus:.nbs/events`

At runtime, the AI modifies the registry by writing commands to `.nbs/control-inbox-<handle>`.

### Control Commands

Written to `.nbs/control-inbox-<handle>`, one per line:

| Command | Effect |
|---------|--------|
| `register-chat <path>` | Add a chat file to the watch list |
| `unregister-chat <path>` | Remove a chat file |
| `register-bus <path>` | Add a bus events directory |
| `unregister-bus <path>` | Remove a bus events directory |

The control inbox is append-only. The sidecar tracks a line offset and only processes new lines. Full history preserved for audit.

## Cleanup

On exit (INT, TERM, or normal), the sidecar process is killed and the nbs-ts session is cleaned up. The control inbox and registry persist for post-session analysis.

## File Layout

```
.nbs/
├── control-inbox-<handle>    # AI → sidecar commands (append-only)
├── control-registry-<handle> # Current resource watch list
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
| 1 | General error (nbs-ts not found, session creation failed) |
| 4 | Invalid arguments |

## See Also

- [nbs-sidecar](nbs-sidecar.md) — the background monitor process
- [Coordination](../concepts/coordination.md) — why event-driven coordination matters
- [nbs-bus](nbs-bus.md) — event queue that the sidecar monitors
