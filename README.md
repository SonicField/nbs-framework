# NBS Framework

**No Bull Shit.** AI systems are fluent, confident, and wrong often enough to be dangerous. NBS makes every claim carry its own falsifier — so the moment something breaks, you know it, instead of discovering it three decisions later when the damage is irreversible.

## Documentation

- [Why NBS](docs/Why-NBS.md) - The philosophy: falsifiability over bullshit
- [Overview](docs/overview.md) - Why this exists and how it works
- [Getting Started](docs/getting-started.md) - Installation and first use
- [NBS Teams](docs/nbs-teams.md) - Supervisor/worker patterns for multi-agent work
- [NBS Chat](docs/nbs-chat.md) - File-based AI-to-AI chat for worker coordination
- [Testing Strategy](docs/testing-strategy.md) - AI-evaluates-AI testing approach
- [Interactive Testing](docs/interactive-testing.md) - Multi-turn testing with pty-session
- [pty-session Reference](docs/pty-session.md) - Terminal session manager for automation
- [Style Guide](docs/STYLE.md) - Internal reference for AI writing these materials
- [Document Tools](docs/nbs-doc.md) - Analysis, planning, and description tools

## Examples

- [CLAUDE.md](examples/CLAUDE.md) - Example project configuration for NBS programming

## Foundation

- [Goals](concepts/goals.md) - The why. Everything else exists in service of this.

## Pillars

Built on the foundation:

- [Falsifiability](concepts/falsifiability.md) - Claims require potential falsifiers
- [Rhetoric](concepts/rhetoric.md) - Ethos, Pathos, Logos and knowing when to ask
- [Verification Cycle](concepts/verification-cycle.md) - Design → Plan → Deconstruct → [Test → Code → Document]
- [Zero-Code Contract](concepts/zero-code-contract.md) - Engineer specifies, Machinist implements with evidence
- [Bullshit Detection](concepts/bullshit-detection.md) - Honest reporting, negative outcome analysis

## Tools

### Core Command

- [/nbs](claude_tools/nbs.md) - Review and dispatch

Run this after any session. It detects context and dispatches:
- In `investigation/*` branch → reviews investigation rigour
- After `/nbs-discovery` → verifies the discovery report is complete
- After `/nbs-recovery` → reviews the recovery work
- Otherwise → general NBS review

### NBS Teams Tools

Supervisor/worker patterns for multi-agent AI work. See [NBS Teams](docs/nbs-teams.md) for the full overview.

The communication layer is built on two primitives: **chat** (file-based messaging) and **bus** (file-based events). Chat files are plain text with base64-encoded messages, written atomically under `flock()`. The bus uses individual event files — publishing is an atomic write-and-rename, acknowledging moves files to a `processed/` directory. No daemons, no databases, no sockets. When a machine dies, the messages survive. When a session restarts, the queue is intact.

This architecture enables direct worker-to-worker coordination without routing everything through a supervisor. A team of 12 AI agents used this system to exchange 12,803 messages over 28 days on a real compiler project, producing 374 commits with 84 self-corrections logged across 2,604 decisions. The full analysis is in [The Ant and the Anthill](blog/The-Ant-And-The-Anthill.md).

Commands for setting up and using NBS teams:

- [/nbs-teams-start](claude_tools/nbs-teams-start.md) - Bootstrap project with `.nbs/` structure (one command setup)
- [/nbs-teams-help](claude_tools/nbs-teams-help.md) - Interactive guidance for NBS teams usage
- [/nbs-help](claude_tools/nbs-help.md) - Interactive guidance for the NBS framework

For AI-as-supervisor or AI-as-worker roles:
- [NBS Supervisor](claude_tools/nbs-supervisor.md) - Role and responsibilities for supervisor
- [NBS Worker](claude_tools/nbs-worker.md) - Role and responsibilities for worker
- [NBS Testkeeper](claude_tools/nbs-testkeeper.md) - Role and responsibilities for testkeeper
- [NBS Gatekeeper](claude_tools/nbs-gatekeeper.md) - Role and responsibilities for gatekeeper
- [NBS Theologian](claude_tools/nbs-theologian.md) - Role and responsibilities for theologian (theory and architecture)
- [NBS Teams Chat](claude_tools/nbs-teams-chat.md) - File-based AI-to-AI chat for worker coordination

### Workflow Commands

- [/nbs-discovery](claude_tools/nbs-discovery.md) - Read-only archaeology for messy projects
- [/nbs-recovery](claude_tools/nbs-recovery.md) - Step-wise restructuring with confirmation

### Document Tools

Tools for working with documents - analysing, planning, and describing:

- [/nbs-doc-help](claude_tools/nbs-doc-help.md) - Interactive guidance for document tools
- [/nbs-doc-analyse](claude_tools/nbs-doc-analyse.md) - Detect BS, find actual vs stated goals
- [/nbs-doc-plan](claude_tools/nbs-doc-plan.md) - Plan documents before writing
- [/nbs-doc-describe](claude_tools/nbs-doc-describe.md) - Help describe systems, code, concepts

See [Document Tools](docs/nbs-doc.md) for the full overview.

### Side Quest Commands

- [/nbs-investigation](claude_tools/nbs-investigation.md) - Hypothesis testing through experiment (isolated side branch)

Run this when you want to test a hypothesis before committing to a direction. Creates an isolated investigation branch, designs falsifiable experiments, and produces a verdict (falsified / failed to falsify / inconclusive).

### Verification Commands

- [/nbs-discovery-verify](claude_tools/nbs-discovery-verify.md) - Verify discovery report completeness (auto-dispatched by /nbs)

### Operational Tools

- [/nbs-audit](claude_tools/nbs-audit.md) - Audit codebase against engineering standards with parallel sub-agents
- [/nbs-poll](claude_tools/nbs-poll.md) - Periodic check of chats and workers (heartbeat)
- [/nbs-chat-digest](claude_tools/nbs-chat-digest.md) - Summarise chat channel history
- [/nbs-pte](claude_tools/nbs-pte.md) - Precise Technical English mode for unambiguous specifications
- [/nbs-natural](claude_tools/nbs-natural.md) - Exit Precise Technical English mode

## Sub-Projects

### Terminal Weathering

Progressive replacement of CPython call protocol paths with C type slot implementations, using NBS principles. Evidence from initial Rust/PyO3 work showed that function body replacement leaves CPython's dispatch overhead intact — the performance-critical layer is the call protocol, which requires direct C access to type slots. The methodology (evidence gates, falsifiability, progressive replacement) is unchanged; the unit of work shifted from function bodies to type slots, and the implementation language from Rust to C against CPython's type API, with ASan, leak analysis, and refcount verification as mandatory correctness gates.

- [Terminal Weathering Documentation](terminal-weathering/docs/) - Theory, getting started, methodology
- [Concept](terminal-weathering/concepts/terminal-weathering.md) - The philosophy and phases
- [Evidence](terminal-weathering/evidence/) - Measured data supporting the Rust-to-C pivot
- [/nbs-terminal-weathering](claude_tools/nbs-terminal-weathering.md) - The tool command

## Testing

The framework includes automated tests using a novel AI-evaluates-AI approach, plus unit and integration tests for all C binaries.

```bash
make test-unit     # 70+ unit tests across bus, chat, sidecar
make test          # Integration tests (lifecycle, interrupt, auto-archive, + component tests)
make test-all      # Everything
```

- [Testing Strategy](docs/testing-strategy.md) - Philosophy, adversarial testing, test isolation
- [Interactive Testing](docs/interactive-testing.md) - Using pty-session for multi-turn tests
- [pty-session Reference](docs/pty-session.md) - Interactive terminal session manager (REPLs, debuggers)
- [nbs-worker Reference](docs/nbs-worker.md) - Worker lifecycle management (spawn, monitor, search, dismiss)

See [tests/README.md](tests/README.md) for details.

## Planning

Project plans and progress logs live in `planning/`:
- `<date>-<project>-plan.md` - Terminal goal, completed/outstanding items, decisions
- `<date>-<project>-progress.md` - Session-by-session record of what was done

## Installation

### Prerequisites

- GCC (C11 support) or Clang
- GNU Make
- tmux (runtime dependency for session management)

### Quick Start

```bash
git clone https://github.com/SonicField/nbs-framework.git
cd nbs-framework
make && make install
./bin/install.sh
```

That's it. Three commands: clone, build, install.

### What Gets Built

`make` builds all five C components from `src/`:

| Component | Binary | Purpose |
|-----------|--------|---------|
| nbs-bus | `bin/nbs-bus` | File-based event queue for agent coordination |
| nbs-chat | `bin/nbs-chat`, `bin/nbs-chat-terminal`, `bin/nbs-chat-remote` | Chat file protocol (create, send, read, poll, search, export) |
| nbs-sidecar | `bin/nbs-sidecar` | Background monitor for Claude Code sessions (20 behaviours) |
| nbs-pty-session | `bin/pty-session` | Terminal session manager (create, send, read, wait, kill) |
| nbs-worker | `bin/nbs-worker` | Worker lifecycle management (spawn, status, search, dismiss) |

All binaries are compiled with `-Wall -Wextra -Wshadow -Werror -std=c11`. Assertions are always-on (not gated by NDEBUG).

`make install` copies binaries to `bin/`.

`./bin/install.sh` creates `~/.nbs/` with processed commands and symlinks in `~/.claude/commands/`.

For custom install location: `./bin/install.sh --prefix=/path/to/location`

### Running Tests

```bash
make test          # Integration tests for all components
make test-unit     # Unit tests only (bus + chat + sidecar)
make test-all      # Unit tests + integration tests
```

### Build Modes

```bash
make debug         # Debug build (-O0 -g, for gdb)
make               # Release build (-O2, assertions ON)
```

ASan builds are available per-component: `make -C src/nbs-sidecar asan`

### Remaining Bash Scripts

The `bin/` directory also contains bash scripts for user-facing tools that are not performance-critical:

- `nbs-claude` — Thin wrapper (~300 lines) that launches Claude Code + nbs-sidecar
- `nbs-sidecar-restart` — Hot-restart running sidecars to pick up new binary
- `nbs-prompts` — Multilingual standup prompt generation
- `nbs-remote-*` — SSH proxy tools for remote operations
- `pty-session-lock` — Advisory locking for pty sessions
- `nbs-chat-init` — Project initialisation

These are orchestration scripts, not hot-path code. The sidecar, chat, bus, pty-session, and worker — all called per-tick or per-agent-action — are C binaries.

## Author

Dr Alex Turner

## Licence

[MIT](LICENSE)
