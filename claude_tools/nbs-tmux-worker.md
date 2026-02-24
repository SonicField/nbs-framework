---
description: Manage NBS worker lifecycle - spawn, monitor, search, dismiss
allowed-tools: Bash
---

# NBS Worker Management (nbs-workers)

This skill enables spawning and managing Claude worker instances with persistent logging, unique naming, and integrated task file management.

---

## Commands

```bash
nbs-workers spawn <slug> <project-dir> <task-description>
nbs-workers status <name>
nbs-workers search <name> <regex> [--context=N]
nbs-workers results <name>
nbs-workers dismiss <name>
nbs-workers list
nbs-workers help
```

### spawn

Create task file, start Claude worker, send initial prompt. Returns the generated worker name.

```bash
nbs-workers spawn parser /path/to/project "Implement the parser. Pass all 84 tests."
# Output: parser-a3f1
```

What happens:
1. Generates unique name: `<slug>-<4-char-hash>` (e.g., `parser-a3f1`)
2. Creates task file at `.nbs/workers/<name>.md`
3. Starts tmux session with persistent logging
4. Launches Claude and sends the task prompt

### status

Report worker status combining tmux session state and task file State field.

```bash
nbs-workers status parser-a3f1
```

Output:
```
Worker: parser-a3f1
  tmux session: yes
  task state:   running
  status:       running
```

Status truth table:

| tmux alive | State field | Reported status |
|-----------|-------------|-----------------|
| yes | running | running |
| yes | completed | completed (session still open) |
| no | running | died (session exited unexpectedly) |
| no | completed | completed |
| no | failed/escalated | failed/escalated |

### search

Search persistent log for regex matches with surrounding context lines.

```bash
nbs-workers search parser-a3f1 "ERROR.*test" --context=20
```

- Default context: 50 lines before and after each match
- ANSI escape codes are stripped before matching
- Uses extended regex (grep -E)
- Works even after session has exited (log is persistent)

### results

Extract the Log section from a completed worker's task file.

```bash
nbs-workers results parser-a3f1
```

### dismiss

Kill the tmux session and mark the task file as dismissed. Log file is preserved.

```bash
nbs-workers dismiss parser-a3f1
```

### list

Show all workers with status summary.

```bash
nbs-workers list
```

Output:
```
NBS Workers:
  parser-a3f1               running      tmux:alive log:45230B
  docs-review-7b2c          completed    tmux:dead  log:12400B
```

---

## Usage Pattern

### Spawning a Worker

```bash
# Spawn returns the unique worker name
WORKER=$(nbs-workers spawn parser /path/to/project "Implement the parser. Pass all 84 tests.")
echo "Spawned: $WORKER"
```

### Monitoring Progress

```bash
# Check status
nbs-workers status parser-a3f1

# Search log for progress indicators
nbs-workers search parser-a3f1 "test.*pass" --context=5

# Search for errors
nbs-workers search parser-a3f1 "ERROR|FAIL|error" --context=10
```

### Reading Results

```bash
# When status shows completed, read the results
nbs-workers results parser-a3f1
```

### Cleanup

```bash
# Dismiss when done reviewing
nbs-workers dismiss parser-a3f1
```

---

## File Layout

```
.nbs/workers/
├── parser-a3f1.md    # Task file (status, log, results)
├── parser-a3f1.log   # Persistent session output
├── docs-7b2c.md      # Another worker's task file
└── docs-7b2c.log     # Another worker's log
```

---

## Location

The `nbs-workers` script is at: `{{NBS_ROOT}}/bin/nbs-workers`

Ensure it's in your PATH or use the full path.
