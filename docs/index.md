# NBS Framework Documentation

## [Getting Started](getting-started/index.md)

Tutorial: install, configure, launch your first team, understand the system.

## [Framework](framework/)

The epistemic methodology behind NBS — why it exists and how it thinks.

| Document | Description |
|----------|-------------|
| [Why NBS](framework/Why-NBS.md) | Motivation and core principles |
| [Overview](framework/overview.md) | System architecture at a glance |
| [Pillars](framework/pillars.md) | The seven concept files: goals, falsifiability, rhetoric, bullshit detection, verification, zero-code contract, engineering standards |
| [Skill System](framework/skill-system.md) | How skills work: slash commands, installation, creation |
| [/nbs Review](framework/nbs-review.md) | The review skill and its dynamic dispatch system |
| [/nbs-audit](framework/nbs-audit.md) | Codebase audit against engineering standards |
| [Investigation Mode](framework/investigation.md) | Hypothesis-driven side quests for debugging |
| [Discovery & Recovery](framework/discovery.md) | Collaborative exploration and restructuring of unstructured projects |
| [Document Tools](framework/nbs-doc.md) | Writing tools: analyse, plan, describe |
| [Style Guide](framework/STYLE.md) | Writing conventions |
| [Wiki Style](framework/WIKI_STYLE.md) | Wiki-specific formatting |

## [Tools](tools/)

Reference documentation for infrastructure components.

| Document | Description |
|----------|-------------|
| [Tool Reference](tools/tools.md) | Quick reference for all CLI tools |
| [nbs-chat-terminal](tools/nbs-chat-terminal.md) | Interactive terminal: all commands, watchdog, oracles, line editing |
| [nbs-chat-init](tools/nbs-chat-init.md) | Project initialisation: phases, scorched earth, setup |
| [nbs-chat](tools/nbs-chat.md) | Chat file format, read/write/search commands |
| [nbs-chat-edit](tools/nbs-chat-edit.md) | Interactive chat editor: view, search, delete messages |
| [nbs-claude](tools/nbs-claude.md) | Claude wrapper: session management, debug mode |
| [nbs-sidecar](tools/nbs-sidecar.md) | Background monitor: notifications, triggers, transport |
| [nbs-ts](tools/nbs-ts.md) | Session management and nbs-ts-helper daemon |
| [nbs-ts-grep](tools/nbs-ts-grep.md) | Search across active session output |
| [nbs-ts-render](tools/nbs-ts-render.md) | Virtual terminal renderer — PTY output to plain text |
| [nbs-spawn-worker](tools/nbs-spawn-worker.md) | Worker spawn pipeline and nbs-launch-agent |
| [nbs-bus](tools/nbs-bus.md) | Event bus: publish, subscribe, dedup |
| [nbs-bus Recovery](tools/nbs-bus-recovery.md) | Bus failure modes and recovery |
| [nbs-oracle-reaper](tools/nbs-oracle-reaper.md) | Stateless oracle lifecycle management |
| [nbs-hub](tools/nbs-hub.md) | Process enforcement for teams |
| [nbs-scribe-log](tools/nbs-scribe-log.md) | Decision log binary (append-only) |
| [nbs-workers](tools/nbs-workers.md) | Worker lifecycle: spawn, status, dismiss |
| [Remote Tools](tools/nbs-remote.md) | nbs-remote-run/edit/read and nbs-local-run |
| [Testing Strategy](tools/testing-strategy.md) | Test philosophy and methodology |
| [Interactive Testing](tools/interactive-testing.md) | Manual testing procedures |

## [Team](team/)

Running and operating agent teams.

| Document | Description |
|----------|-------------|
| [Teams Overview](team/nbs-teams.md) | Team structure, roles, operations |
| [Tripod Architecture](team/tripod-architecture.md) | Scribe, Pythia, and the decision-logging triangle |
| **Permanent agents** | |
| [Supervisor](team/nbs-supervisor.md) | Goal-keeper, task delegation, session boundaries |
| [Generalist](team/nbs-generalist.md) | Implementation worker |
| [Gatekeeper](team/nbs-gatekeeper.md) | Pre-push code review (read-only) |
| [Theologian](team/nbs-theologian.md) | Architecture and design guidance |
| [Testkeeper](team/nbs-testkeeper.md) | Test ownership and falsification |
| [Scribe](team/nbs-scribe.md) | Decision logging and institutional memory |
| [Medic](team/nbs-medic.md) | Continuous hallucination monitor |
| **Ephemeral oracles** | |
| [Pythia](team/nbs-pythia.md) | Trajectory and risk assessment |
| [Shepard](team/nbs-shepard.md) | Team effectiveness assessment |
| [Librarian](team/nbs-librarian.md) | Institutional memory watchdog |
| [Fixup](team/nbs-fixup.md) | Team self-repair |
| | |
| [Help When Stuck](team/help-when-stuck.md) | Troubleshooting guide for stuck teams |

## [Examples](../examples/)

Complete worked sessions showing the framework in action.

| Example | Description |
|---------|-------------|
| [TSP Session](../examples/tsp-session/README.md) | Team tackles an impossible problem — goal revision, five falsified hypotheses, useful deliverables |
