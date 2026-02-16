# NBS Teams

Supervisor/worker pattern for multi-agent AI work.

## The Problem

Context accumulates. Drift follows. You start on problem A, get distracted by sub-problem B, emerge hours later having solved neither.

Workers fix this. Fresh context, specific task, success criteria. Execute and report. The supervisor holds the terminal goal.

## The Pattern

```
Supervisor (you or Claude)
    │
    ├── Worker 1: "Implement the parser. Pass all 84 tests."
    │       └── Fresh context, executes, reports findings
    │
    ├── Worker 2: "Update documentation to match new API."
    │       └── Fresh context, executes, reports findings
    │
    └── Worker 3: "Run adversarial tests on edge cases."
            └── Fresh context, executes, reports findings
```

The supervisor:
- Maintains terminal goal clarity
- Decomposes work into worker tasks
- Spawns workers (via nbs-worker or Task tool sub-agents)
- Captures learnings after each worker (3Ws)
- Runs self-check every 3 workers
- Coordinates via chat — Scribe captures decisions, Pythia assesses trajectory

Workers:
- Read their task file
- Execute with fresh context
- Report findings with evidence
- Escalate blockers (no workarounds)

## Task Scope

**Wrong** (micromanagement):
```
Worker 1: Implement parse_int()
Worker 2: Implement parse_string()
Worker 3: Implement parse_block()
```

**Right** (delegation):
```
Worker: Implement the parser. Pass all 84 tests.
```

If you're writing implementation steps, scope is too narrow. Set the goal, let workers choose the path.

## Directory Structure

```
.nbs/
├── chat/
│   └── live.chat           # Coordination via chat
├── scribe/
│   └── live-log.md         # Decision log (managed by Scribe)
├── events/
│   └── config.yaml         # Bus configuration
└── workers/
    ├── parser-a3f1.md      # Task file (created by nbs-worker spawn)
    ├── parser-a3f1.log     # Persistent session output
    └── ...
```

## Commands

| Command | Purpose |
|---------|---------|
| `/nbs-teams-start` | Bootstrap `.nbs/` structure |
| `/nbs-teams-help` | Interactive guidance - ask Claude for help |
| `/nbs-teams-supervisor` | Supervisor role reference |
| `/nbs-teams-worker` | Worker role reference |
| `/nbs-teams-chat` | AI-to-AI chat reference |
| `/nbs-teams-fixup` | Diagnose and restart stalled agents |
| `/nbs-tmux-worker` | nbs-worker command reference |

### Getting Help

Run `/nbs-teams-help` when stuck. It asks what you need help with:
- Starting a project
- Spawning workers
- Writing task files
- Task scope
- Monitoring
- 3Ws and self-check

Claude walks you through interactively, using your actual project for examples if `.nbs/` exists. Mentor, not manual.

## Quick Start

1. Run `/nbs-teams-start`
2. Answer the terminal goal question
3. Decompose into worker tasks
4. Spawn workers with `nbs-worker spawn` or Task tool sub-agents
5. Capture 3Ws after each completes

## 3Ws

After every worker:

- **What went well** - Keep doing this
- **What didn't work** - Stop doing this
- **What we can do better** - Change this

After every 3 workers, run self-check:
- Am I still pursuing terminal goal?
- Am I delegating vs doing tactical work myself?
- Have I captured learnings?
- Should I escalate to human?

## When to Use

Use NBS teams when:
- Task requires multiple distinct phases
- Context accumulation is causing drift
- You want fresh perspectives on sub-problems
- Work can be parallelised

Don't use when:
- Single coherent task
- Deep context is the asset (investigation, debugging)
- Task is trivial

## Coordination Bus

For projects where multiple agents need to react to each other's events (task completion, chat mentions, human input), the coordination bus provides event-driven notification. Instead of polling chat files and worker directories repeatedly, agents check a priority-ordered event queue.

```bash
# Create the bus
mkdir -p .nbs/events/processed

# Workers publish events automatically on state changes
# Supervisor checks the queue on each poll cycle
nbs-bus check .nbs/events/
```

The bus is optional. Projects without `.nbs/events/` fall back to direct chat/worker scanning via `nbs-poll`. See [nbs-bus](nbs-bus.md) for the full reference and [Bus Recovery](nbs-bus-recovery.md) for startup/restart protocol.

## See Also

- [nbs-bus](nbs-bus.md) - Event-driven coordination bus
- [nbs-chat](nbs-chat.md) - File-based AI-to-AI chat for worker coordination
- [nbs-worker](nbs-worker.md) - Worker lifecycle management (spawn, monitor, search, dismiss)
- [Tripod Architecture](tripod-architecture.md) - Scribe (institutional memory) and Pythia (risk oracle)
- [Why NBS](Why-NBS.md) - The philosophy behind the framework
