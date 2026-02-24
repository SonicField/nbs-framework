# nbs-workers: Worker Lifecycle Management

nbs-workers manages Claude worker instances for NBS teams with persistent logging, unique naming, and integrated task files.

## How It Works

nbs-workers wraps tmux with a higher-level abstraction:

- **Unique names** — Generated as `<slug>-<4-char-hash>` (e.g., `parser-a3f1`) to avoid collisions across supervisor sessions
- **Persistent logging** — Uses `tmux pipe-pane` to stream all output to `.nbs/workers/<name>.log` from session start. Output survives any exit (natural or killed)
- **Task file integration** — Automatically creates `.nbs/workers/<name>.md` with task description and status tracking
- **ANSI stripping** — Search command strips terminal escape codes before matching

## Commands

| Command | Purpose |
|---------|---------|
| `nbs-workers spawn <slug> <dir> <desc>` | Create task file, start Claude, send prompt |
| `nbs-workers status <name>` | tmux alive + State field from task file |
| `nbs-workers search <name> <regex> [--context=N]` | Search persistent log (default context: 50) |
| `nbs-workers results <name>` | Extract Log section from task file |
| `nbs-workers dismiss <name>` | Kill session, mark dismissed, preserve log |
| `nbs-workers list` | All workers with status summary |
| `nbs-workers help` | Usage reference |

## Quick Start

```bash
# Spawn a worker
WORKER=$(nbs-workers spawn parser /path/to/project "Implement the parser. Pass all 84 tests.")

# Check status
nbs-workers status $WORKER

# Search for progress
nbs-workers search $WORKER "test.*pass" --context=10

# Read completed results
nbs-workers results $WORKER

# Dismiss when done
nbs-workers dismiss $WORKER
```

## Status Logic

The `status` command combines tmux session state with the task file's `State:` field:

| tmux alive | State field | Reported status |
|-----------|-------------|-----------------|
| yes | running | running |
| yes | completed | completed (session still open) |
| no | running | died (session exited unexpectedly) |
| no | completed | completed |
| no | failed/escalated | failed/escalated |

## File Layout

```
.nbs/workers/
├── parser-a3f1.md    # Task file (status, task description, log)
├── parser-a3f1.log   # Persistent session output (full history)
├── docs-7b2c.md
└── docs-7b2c.log
```

## Bus Integration

When `.nbs/events/` exists, `nbs-workers` lifecycle commands automatically publish bus events:

| Command | Bus event type | Priority |
|---------|---------------|----------|
| `spawn` | `worker-spawned` | `normal` |
| `dismiss` | `worker-dismissed` | `normal` |
| `status` (detects died) | `worker-died` | `high` |

A `worker-died` event is published when `status` finds the tmux session dead but the task file still shows state `running`. This allows the supervisor to detect and respond to unexpected worker exits.

If `.nbs/events/` does not exist, publishing is silently skipped — `nbs-workers` works without the bus.

See [nbs-bus](nbs-bus.md) for the full bus reference.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error / no matches |
| 2 | Worker not found |
| 4 | Invalid arguments |

## Location

```
bin/nbs-workers
```

Installed to `~/.nbs/bin/` by `bin/install.sh` (symlinked with the rest of `bin/`).
