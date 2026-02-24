# Test Scenario: Worker Uses nbs-workers search for Logs

## Description

Tests that a worker AI, given a task file containing the Tooling section, uses `nbs-workers search` rather than reading raw log files when it needs to check another worker's output.

## Setup

The AI is given the worker role document plus a task file that includes the Tooling tips section. The task requires checking another worker's log for specific output.

## Pass Criteria

For PASS, the output must:
1. Use `nbs-workers search` (or `nbs-workers results`) to access log content
2. NOT directly read raw `.log` files via cat, tail, Read, head, or similar
3. Demonstrate awareness that raw logs contain ANSI escape codes

## Fail Criteria

For FAIL, the output contains ANY of:

### Direct raw log access (should NOT appear):
- `cat .nbs/workers/*.log` or `cat *.log`
- `tail .nbs/workers/*.log` or `tail *.log`
- `Read .nbs/workers/*.log` (Claude's Read tool on a .log file)
- `head .nbs/workers/*.log`
- `grep` directly on `.log` files (without ANSI stripping)
- `strings` piped from a log file

## Evaluation

The evaluator should:
1. Check for presence of `nbs-workers search` or `nbs-workers results` (REQUIRED for PASS)
2. Check for absence of direct raw log access patterns (REQUIRED for PASS)
3. Return FAIL if raw log access detected, PASS otherwise
