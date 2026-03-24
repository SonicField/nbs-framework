# Testing the NBS Framework

This directory contains tests for the NBS framework commands.

## Testing Philosophy

AI output is non-deterministic. Testing it requires semantic evaluation, not string matching.

**The approach**:
1. Run the command on a known scenario
2. A second AI instance evaluates the output against explicit criteria
3. The verdict is written to a JSON file (deterministic state of truth)
4. Exit code reflects the verdict

This means tests are falsifiable: they fail if the evaluator determines the output missed known issues or incorrectly assessed the scenario.

---

## Directory Structure

```
tests/
├── automated/           # AI-evaluated tests (run with bash)
│   ├── scenarios/       # Test scenarios with known ground truth
│   │   ├── no_plan_project/    # Project missing plan/progress files
│   │   ├── messy_project/      # Scattered artefacts for discovery
│   │   ├── post_discovery/     # Valid discovery report for dispatch testing
│   │   ├── investigation/      # Investigation context with status file
│   │   └── bad_discovery/      # Deliberately bad report (should-fail test)
│   ├── verdicts/        # Generated test outputs (git-ignored)
│   └── test_*.sh        # Test scripts
└── manual/              # Human-executed QA procedures
    ├── qa_nbs.md        # QA script for /nbs command
    └── qa_discovery.md  # QA script for /nbs-discovery
```

---

## Automated Tests

### Running Tests

```bash
# Run the full test suite (auto-discovers all test_*.sh in automated/)
./tests/run_all.sh

# Quick mode — skip AI-evaluated tests
./tests/run_all.sh --quick

# Run only tests matching a substring
./tests/run_all.sh --target=nbs_ts

# C unit tests only (fast, <10s)
make test-unit

# Run a single test directly
bash tests/automated/test_nbs_chat_lifecycle.sh
```

`run_all.sh` auto-discovers all `test_*.sh` files in `automated/`. New tests are included automatically — you do not need to edit `run_all.sh` when adding a test.

### Test Descriptions

| Test | Purpose | Falsification |
|------|---------|---------------|
| `test_install.sh` | Verifies symlinks created correctly | Fails if any command symlink missing or incorrect |
| `test_nbs_command.sh` | Tests `/nbs` on project missing plan | Fails if known issues not identified |
| `test_nbs_discovery.sh` | Tests `/nbs-discovery` on messy scenario | Fails if artefacts not found or incorrectly triaged |
| `test_nbs_recovery.sh` | Tests `/nbs-recovery` plan generation | Fails if plan missing required properties |
| `test_nbs_dispatch.sh` | Tests dispatch to verification after discovery | Fails if normal review produced instead of verification |
| `test_dispatch_adversarial.sh` | Tests NO dispatch without discovery context | Fails if verification mode incorrectly triggered |
| `test_investigation_branch.sh` | Tests dispatch via investigation/* branch (P0) | Fails if branch not detected or normal review produced |
| `test_investigation_file.sh` | Tests dispatch via INVESTIGATION-STATUS.md at root (P1) | Fails if file not detected or normal review produced |
| `test_investigation_ask.sh` | Tests ask behaviour when file in subdirectory (P1) | Fails if AI proceeds without asking |
| `test_investigation_adversarial.sh` | Tests NO investigation dispatch without markers (P0) | Fails if investigation review incorrectly triggered |
| `test_investigation_adv_no_normal.sh` | Tests file at root does NOT produce normal review (P2) | Fails if normal review produced instead of investigation review |
| `test_investigation_adv_no_silent.sh` | Tests file in subdirectory does NOT silently proceed (P2) | Fails if AI produces complete review without asking |
| `test_pty_session_lifecycle.sh` | Tests nbs-ts (pty-session) create/send/read/kill cycle | Fails if any operation errors |
| `test_pty_session_wait.sh` | Tests pty-session wait pattern detection | Fails if wait doesn't detect pattern |
| `test_pty_session_timeout.sh` | Tests pty-session wait timeout | Fails if timeout doesn't work |
| `test_pty_session_adv_no_collision.sh` | Tests pty-session isolation from user sessions | Fails if user sessions affected |
| `test_pty_session_adv_invalid.sh` | Tests pty-session error handling | Fails if invalid input crashes |
| `test_evaluator_catches_bad.sh` | Meta-test: evaluator catches bad reports | Fails if evaluator passes a known-bad report |
| `test_nbs_worker_lifecycle.sh` | Tests nbs-workers spawn/status/search/results/dismiss cycle | Fails if any lifecycle operation errors |
| `test_nbs_worker_search.sh` | Tests nbs-workers search with ANSI stripping and context | Fails if search misses markers or context is wrong |
| `test_supervisor_nbs_worker.sh` | Tests supervisor uses nbs-workers for spawning | Fails if AI doesn't use nbs-workers commands |
| `test_supervisor_adv_no_old_pattern.sh` | Tests supervisor does NOT use old pty-session spawn pattern | Fails if temp.sh or raw pty-session create/send pattern detected |
| `test_help_nbs_worker.sh` | Tests help skill recommends nbs-workers for spawning | Fails if help recommends old pty-session pattern |

### Scenarios

**no_plan_project/**: A project with code but no plan or progress files. Used to test `/nbs` detection of missing documentation.

**messy_project/**: Scattered artefacts with known ground truth. Contains:
- `GROUND_TRUTH.md` - What the test knows (hidden from discovery)
- Multiple loader versions (v1 broken, v2 working, v3 incomplete)
- Used to test `/nbs-discovery` artefact detection

**post_discovery/**: A valid discovery report for testing dispatch behaviour.
- `discovery_report.md` - Complete report with all required sections
- Used to test `/nbs` dispatch to verification mode

**bad_discovery/**: Deliberately incomplete discovery report.
- Missing sections, wrong verdicts
- Used to verify the evaluator catches errors (should-fail test)

**investigation/**: Investigation context with status file.
- `INVESTIGATION-STATUS.md` - Mock investigation in progress
- Used to test `/nbs` dispatch to investigation review mode

**supervisor_nbs_worker/**: Minimal supervisor context for nbs-workers tests.
- Tests that supervisor role uses nbs-workers for worker management
- Tests that old pty-session spawn pattern is not used

### Verdict Files

Tests write output to `verdicts/` subdirectory:

| File | Contents |
|------|----------|
| `*_verdict.json` | Evaluator judgement (PASS/FAIL with reasoning) |
| `*_output.txt` | Raw command output for debugging |

**Why a separate directory?**
- Tests need a writable location with predictable permissions
- Keeping generated files separate from scenarios avoids confusion
- All files in `verdicts/` are git-ignored (only the README is tracked)

**Structure of a verdict file:**
```json
{
  "verdict": "PASS",
  "criteria_met": { ... },
  "reasoning": "Brief explanation"
}
```

The exit code is derived from the verdict. These files persist for debugging failed tests.

---

## Manual Tests

Manual QA procedures for human evaluation. Use these on real projects.

### qa_nbs.md

QA script for the `/nbs` command. Checks:
- Foundation awareness (goals.md read or understood)
- Output structure (Status, Issues, Recommendations)
- Honest assessment (real issues, not invented or omitted)
- Questions asked (uses AskUserQuestion when needed)
- Pillar depth (reads pillars when appropriate, not wastefully)

### qa_discovery.md

QA script for `/nbs-discovery`. Run on a real project with the project owner. Produces:
- Discovery report (artefacts, triage, gap analysis)
- Process log (what worked, what didn't)

Used to evaluate the framework on real-world messiness, not synthetic scenarios.

---

## Adding New Tests

Create a new `test_*.sh` file in `tests/automated/`. It will be auto-discovered by `run_all.sh` on the next run — no registration needed.

If the test requires AI (Claude), add its name to the `AI_TESTS` list in `run_all.sh` so it is skipped with `--quick` and routed through `nbs-ts` when running inside Claude Code.

### Automated Test Pattern

```bash
#!/bin/bash
# Test: [description]
# Falsification: [what causes failure]

set -euo pipefail

# 1. Run the command on a scenario
RESULT=$(claude -p "[prompt]" --output-format text)

# 2. Evaluate with AI
EVAL_PROMPT="[criteria and output to evaluate]"
EVAL_RESULT=$(echo "$EVAL_PROMPT" | claude -p - --output-format text)

# 3. Extract JSON verdict
JSON_VERDICT=$("$EXTRACT_JSON" "$EVAL_TEMP")

# 4. Exit based on verdict
if echo "$JSON_VERDICT" | grep -q '"verdict".*"PASS"'; then
    exit 0
else
    exit 1
fi
```

### Should-Fail Tests

For testing the test infrastructure itself:
1. Create a scenario that should fail (bad report, missing sections)
2. Run the evaluator on it
3. Test passes if evaluator returns FAIL
4. Test fails if evaluator incorrectly returns PASS

---

## Requirements

- Claude Code CLI (`claude`) in PATH
- `bin/extract_json.py` for JSON extraction from AI responses
- Bash with `set -euo pipefail` support
- Python 3 for JSON parsing
