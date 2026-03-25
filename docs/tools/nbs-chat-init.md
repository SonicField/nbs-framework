# nbs-chat-init: Project Bootstrap

Initialises any directory for NBS team operation. Creates the tripod — bus, chat, scribe log — and optionally spawns AI agents via nbs-ts. Run it once per project. Run it again and everything from the previous run is destroyed first.

**Warning:** Re-initialising a project kills all running agents and deletes all state. The confirmation prompt is not decorative.

## Usage

```bash
nbs-chat-init --name=NAME [OPTIONS]
```

The `--name` argument is required. Every resource name — chat file, scribe log, nbs-ts session tags — derives from it.

```bash
cd ~/local/FooBar
nbs-chat-init --name=foobar-dev                          # initialise in cwd
nbs-chat-init --name=phoenix-ts --root=~/local/phoenix   # initialise elsewhere
nbs-chat-init --name=foobar-dev --spawn-only --spawn-all # restart agents only
nbs-chat-init --name=foobar-dev --dry-run                # preview, no side effects
```

## Arguments

| Argument | Required | Description |
|----------|----------|-------------|
| `--name=NAME` | Yes | Chat name. All resource names derive from this. |
| `--root=PATH` | No | Project root (default: cwd). Resolved to absolute. |
| `--project-name=NAME` | No | Override display name (default: `basename` of root). |
| `--force` | No | Skip confirmation prompts. |
| `--dry-run` | No | Print actions without executing. |
| `--spawn-scribe` | No | Launch Scribe in an nbs-ts session. |
| `--spawn-pythia` | No | Launch Pythia in an nbs-ts session. |
| `--spawn-all` | No | Launch Scribe + Pythia + main Claude instance. |
| `--spawn-only` | No | Skip infrastructure creation; spawn agents only. Requires a spawn flag. |
| `--compact-log` | No | Force-archive decision log even if below threshold (100 entries). |

Name validation: `--name` accepts `[a-zA-Z0-9_-]` only. `--project-name` additionally allows dots and spaces. Anything else exits with code 4.

## Phases

### Phase 0: Scorched Earth

Runs before any creation. Kills processes, then deletes files — in that order, so nothing holds files open during removal.

**Process cleanup:** (1) Kill nbs-ts sessions matching the chat name tag. (2) Kill agents tracked in `.nbs/pids/*.pid` (SIGTERM, wait 2s, SIGKILL). (3) Kill sidecars matching `--root=PROJECT_ROOT`.

**File cleanup:**

| What | Path |
|------|------|
| Control files | `.nbs/control-pause`, `.nbs/control-registry-*` |
| PID and session files | `.nbs/pids/*.pid`, `.nbs/sessions/*.json` |
| Trigger timestamps | `.nbs/*-last-run` (librarian, pythia, shepard, fixup) |
| Worker files | `.nbs/workers/*.md`, `.nbs/workers/*.log` |
| Bus events | `.nbs/events/*.event`, `.nbs/events/processed/*.event` |
| Logs | `.nbs/nbs-claude-*.log`, `.nbs/sidecar-*.log` |
| Scribe output | `.nbs/scribe/*.md`, `.nbs/scribe/*.lock` |
| Digests | `.nbs/digests/*.md` |

Everything. Gone.

### Phase 1: Event Bus

Creates `.nbs/events/` and `.nbs/events/processed/`. Writes `config.yaml` with defaults if absent:

| Key | Default | Purpose |
|-----|---------|---------|
| `dedup-window` | 300 | Seconds to suppress duplicate system events |
| `ack-timeout` | 120 | Seconds before unacknowledged events are flagged stale |
| `pythia-interval` | 10 | Scribe decisions between Pythia checkpoints |
| `retention-max-bytes` | 16 MB | Max processed event storage before pruning |

Runs a self-test: publish, acknowledge, verify event reaches `processed/`. If this fails, init aborts.

### Phase 2: Chat

Creates `.nbs/chat/` and `.nbs/chat/archive/`. If a chat file with the same name already exists, it is archived with a timestamp suffix. A fresh file is created via `nbs-chat create` and an init message is posted. Path: `.nbs/chat/<NAME>.chat`.

### Phase 3: Scribe Log

Creates `.nbs/scribe/<NAME>-log.md` with a structured header. If the log exceeds 100 entries (`### D-` headings), it is archived and a fresh log is created pointing to the archive — a linked list of decision history.

### Phase 4: Project Identity and Operational Directories

Writes `.nbs/project-id` (4-character sha256 hash of the absolute project path). Creates `workers/`, `pids/`, `pty-locks/`, `sessions/`. Symlinks from the global `~/.nbs/` installation:

| Symlink | Target | Purpose |
|---------|--------|---------|
| `.nbs/bin/` | `~/.nbs/bin/` | NBS tool binaries |
| `.nbs/commands/` | `~/.nbs/commands/` | Agent skill files |
| `.nbs/docs/` | `~/.nbs/docs/` | Tool reference documentation |

Existing symlinks are kept. Missing global directories produce a warning.

### Phase 5: Health Check

Verifies bus (`nbs-bus status`), chat (`nbs-chat read`), and scribe log (header fields present). If any check fails, init aborts with exit code 1.

### Phase 6: AI Spawn (optional)

Triggered by `--spawn-scribe`, `--spawn-pythia`, or `--spawn-all`. Each agent is launched via `nbs-ts create` with `--dangerously-skip-permissions` — the NBS bus, watchdog, and scribe replace Claude's permission system. The spawner waits up to 60 seconds for CLI readiness before sending the role prompt. An `ai-spawned` bus event is published when all agents are up.

## The `--spawn-only` Mode

Skips phases 0-4. Assumes infrastructure exists. The restart-after-reboot case.

Requires at least one spawn flag. Without one, exits with code 4.

```bash
nbs-chat-init --name=foobar-dev --spawn-only --spawn-all
```

## Chat Name and Session Tags

The `--name` value propagates everywhere:

| Derived from `--name=foo` | Result |
|---------------------------|--------|
| Chat file | `.nbs/chat/foo.chat` |
| Scribe log | `.nbs/scribe/foo-log.md` |
| nbs-ts session tag | `foo` (dots replaced with hyphens) |
| nbs-ts sessions | `nbs-scribe-foo`, `nbs-pythia-foo`, `nbs-claude-foo` |

Phase 0 uses this tag to find and kill previous sessions. Choose a name unique per concurrent project.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error (tool not found, self-test failed, health check failed) |
| 4 | Invalid arguments (missing `--name`, bad characters, `--spawn-only` without spawn flag) |

## Location

```
bin/nbs-chat-init    # Bash script
```

Installed to `~/.nbs/bin/` by `bin/install.sh`.

## See Also

- [nbs-chat](nbs-chat.md) — Chat file protocol and commands
- [nbs-claude](nbs-claude.md) — Agent wrapper with sidecar
- [nbs-bus](nbs-bus.md) — Event bus
- [nbs-bus-recovery](nbs-bus-recovery.md) — Bus startup and restart protocol
