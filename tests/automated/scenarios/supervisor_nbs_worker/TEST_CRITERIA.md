# Test Scenario: Supervisor nbs-workers Adoption

## Description

Tests that an AI loading the updated supervisor role document uses `nbs-workers` commands for worker management, not the old `pty-session` spawn pattern.

## Setup

The AI is given the supervisor role document and asked to spawn a worker for a specific task.

## Pass Criteria

For PASS, the output must:
1. Include `nbs-workers spawn` (or close equivalent showing awareness of the tool)
2. NOT contain the old temp.sh + pty-session spawn pattern
3. NOT contain hedging phrases about nbs-workers availability
4. Demonstrate awareness that nbs-workers handles naming, task files, and logging

## Fail Criteria

For FAIL, the output contains ANY of:

### Old pattern markers (should NOT appear):
- "temp.sh" (the old workaround script)
- "pty-session create" followed by "pty-session send" (raw spawn sequence)
- Creating task files manually before spawning (nbs-workers does this automatically)

### Hedging markers (should NOT appear):
- "if nbs-workers is installed"
- "check if nbs-workers"
- "ensure nbs-workers is available"
- "may not be available"
- "might not be installed"
- "verify nbs-workers exists"

## Evaluation

The evaluator should:
1. Check for presence of `nbs-workers` commands (REQUIRED for PASS)
2. Check for absence of old pty-session spawn pattern (REQUIRED for PASS)
3. Check for absence of hedging phrases (REQUIRED for PASS)
4. Return FAIL if old pattern or hedging detected, PASS otherwise
