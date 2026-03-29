# Team Audit — Human Guide

## What It Is

Team Audit is a structured process for improving codebase quality using an NBS team. Instead of scanning every file in parallel (as `nbs-audit` does), the team works through the codebase in zones — groups of related files — fixing issues with test-driven development and documenting the reasoning behind every change.

The key differences from nbs-audit:
- **Zones, not files.** Files that share invariants are audited together, so cross-file issues are caught.
- **Progressive.** The codebase is always stable. One zone is fully hardened before the next begins.
- **Test-driven.** Every fix has a test written first, verified to actually detect the problem.
- **Documented.** Every assertion comes with a comment explaining why it exists.
- **Resumable.** A progress log tracks state, so work can continue across sessions or teams.

## When to Use It

- After a major refactor — verify nothing was silently broken
- Before a release — systematic hardening pass
- When onboarding a new codebase — understand where the gaps are
- Periodically — code quality drifts over time

## How It Works (5-Minute Overview)

The team works in five phases:

```
Phase 1: Survey       → Map the codebase into zones, rank by risk
Phase 2: Audit        → Find violations zone by zone (read-only)
Phase 3: Fix          → TDD per finding: test first, fix second, document third
Phase 4: Gate         → Integration tests, performance check, commit review
Phase 5: Report       → Document reasoning, produce final summary
```

Phase 1 happens once. Phases 2-4 repeat for each zone. Phase 5 happens at the end.

For large codebases, the team works in sprints — a few zones per sprint, with learning from each sprint informing the next.

### What the Team Does

| Role | Contribution |
|------|-------------|
| Theologian | Maps architecture, defines zones and contracts, reviews fixes for structural correctness |
| Generalist | Performs audits, writes tests, applies fixes, documents changes |
| Testkeeper | Verifies every test can actually detect the problem it claims to test |
| Gatekeeper | Reviews commits before push |
| Supervisor | Tracks progress, manages the progress log, coordinates |
| Scribe | Records decisions |
| Medic | Monitors for hallucination and reasoning failures throughout all phases |

### When You Need to Be Involved

The team works autonomously through most of the process. You are needed at these points:

- **Before Phase 1:** Provide the terminal goal (what does "audited and improved" mean for this codebase?)
- **At sprint boundaries (large codebases):** Review progress, decide whether to continue or adjust scope
- **At the end (Phase 5):** Review the final report
- **Push authorisation:** If your goal file includes "do not push without my permission" (recommended), the team will ask before pushing via chat. Your approval must come as a **chat message** — not terminal input, which cannot be independently verified. Both the gatekeeper and supervisor must independently confirm the authorization is from a real human. If the medic raises a concern about the authorization's authenticity, it is treated as revoked — no push proceeds until a new, verified authorization is given. The gatekeeper reviews commits internally; you approve the push

Note: the gatekeeper reviews every commit during Phase 4. You do not need to review individual commits unless you want to — the gatekeeper handles that. Your review points are Phase 5 (final report) and sprint boundaries.

The team will ask you via chat when they need input. You do not need to monitor continuously.

## Setting Up a Goal File

Create a `goal.md` file in your project directory. This is what the team reads to understand what you want.

### Template

```markdown
# Team Audit Goal

## What to Audit
[Path to the codebase or specific directories to audit]

## Terminal Goal
[One sentence: what does "audited and improved" mean for this codebase?]

## Methodology
The team will follow the Team Audit process described in:
[path to team-audit.md]

## Scope
[Any constraints on what to audit or skip]

## Priorities
[What matters most? Security? Stability? Performance? All of the above?]

## Rules
- Do not push without my explicit permission
- Ask me before making architectural changes that affect public API
- [Any project-specific rules]
```

### Example: C Project

```markdown
# Team Audit Goal

## What to Audit
~/projects/mylib/src/

## Terminal Goal
Harden the core library so that no public API function can be called with
invalid arguments without hitting an assertion, and every error path is
tested.

## Methodology
The team will follow the Team Audit process described in:
~/docs/team-audit.md

## Scope
- Audit src/ only (not examples/ or benchmarks/)
- Skip generated files in src/gen/

## Priorities
1. Security: any silent failure on untrusted input is highest priority
2. Stability: missing preconditions on public API
3. Hardening: internal assertions

## Rules
- Do not push without my explicit permission
- Run the full test suite with ASan+UBSan after every zone
- Keep the existing API stable — add assertions, do not change function signatures
```

### Example: Bash Project

```markdown
# Team Audit Goal

## What to Audit
~/infra/deploy/

## Terminal Goal
Ensure all deployment scripts validate their inputs, fail loudly on errors,
and have tests proving they handle missing config and bad environment
variables correctly.

## Methodology
The team will follow the Team Audit process described in:
~/docs/team-audit.md

## Scope
- All .sh files in deploy/ and deploy/lib/
- Skip deploy/vendor/

## Priorities
1. Scripts that run in production (deploy.sh, rollback.sh) — highest priority
2. Scripts that modify infrastructure state
3. Helper scripts and utilities

## Rules
- Do not push without my explicit permission
- Every script must use set -euo pipefail
- Test scripts must not touch real infrastructure — use mocks for external calls
```

### Example: Python Project

```markdown
# Team Audit Goal

## What to Audit
~/projects/dataflow/src/dataflow/

## Terminal Goal
Add precondition and postcondition assertions to all public functions,
eliminate silent error swallowing, and ensure every data transformation
stage validates its input and output.

## Methodology
The team will follow the Team Audit process described in:
~/docs/team-audit.md

## Scope
- src/dataflow/ (core library)
- Skip tests/ (they are the verification, not the subject)

## Priorities
1. Data validation pipeline — bugs here corrupt downstream output silently
2. External API integrations — error handling on network calls
3. Internal utilities

## Rules
- Do not push without my explicit permission
- Run pytest with -x (fail fast) and hypothesis for property-based tests
- Do not add runtime dependencies
```

## Launching

### Option A: Goal File

Place `goal.md` in your project directory and start a team session. The command requires the chat file path and your handle:

```bash
nbs-chat-terminal .nbs/chat/project.chat yourname --goal-file=goal.md --restart
```

The `--goal-file=` flag injects the goal into chat before the team starts. The `--restart` flag launches the agent team. The supervisor will read the goal and begin Phase 1.

### Option B: Direct Chat

Start a team session without a goal file:

```bash
nbs-chat-terminal .nbs/chat/project.chat yourname --restart
```

Then type your goal as a message to `@team`.

### Option C: Reference from Existing Goal

If you already have a goal file for other work, add a section referencing the team audit:

```markdown
## Next Task
Run a team audit on src/ following the process in ~/docs/team-audit.md.
Focus on security-critical paths first.
```

## Partial-Codebase Auditing

Not every audit targets an entire codebase. If you have added new code to a large existing project — a new subsystem, a feature branch, a library integration — you can audit just the new code and its interactions with the existing code.

In your goal file, describe which code is new (the team will audit it fully) and which existing code it interacts with (the team will audit only the interaction boundaries). The team will use git analysis to determine the scope automatically. The zone map will distinguish **target zones** (new code, full audit) from **context zones** (existing code, boundary-only audit).

This means you can run team-audit on a 5,000-line addition to a million-line codebase without auditing everything.

## Resuming a Previous Audit

Team audit is designed to be resumed across sessions. If a previous session was interrupted or if you want to continue with a new team:

1. Start a new team session pointing at the same project directory
2. The supervisor reads the **audit progress log** to determine where work left off (which zones are hardened, which are in progress, which are unmapped)
3. The team continues from the current phase and zone

The progress log, scribe decision log, and chat history all persist. A new team can pick up exactly where the previous one stopped.

## Checking Progress

The team maintains an **Audit Progress Log** — a structured document tracking:
- Which zones exist and their status (audited, fixing, hardened)
- Findings per zone with severity and fix status
- Invariant coverage (what percentage of identified invariants have tests)
- Backtrack events (when a completed zone was reopened and why)

Ask in chat: `@supervisor what is the current status?` — or read the progress log directly.

## Intervening

You can intervene at any time via chat:

- `@team PAUSE` — stop all work
- `@supervisor skip zone Z3 — low priority` — adjust scope
- `@supervisor focus on security findings first` — change priorities
- `@theologian why did you put X and Y in the same zone?` — question decisions

The team will adapt. You are the engineer; they are the machinists.
