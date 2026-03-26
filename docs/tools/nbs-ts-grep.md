# nbs-ts-grep and nbs-ts-query

Search and extract text from agent session logs. Both tools read raw `output.log` files from `~/.nbs-ts/sessions/`, strip ANSI escapes, and produce clean, searchable text. Only alive sessions are considered.

## nbs-ts-grep

Pattern search across one or all agent sessions within a chat.

### Usage

```
nbs-ts-grep <pattern> <chat-tag> <agent|--all> [--from=N] [--to=N]
```

### What it does

Finds alive nbs-ts sessions by name, reads their `output.log`, strips ANSI control sequences via `strip_ansi`, and prints every line containing the pattern as a substring match.

### Session resolution

The session name is matched by combining the agent name and chat tag. For a specific agent, sessions whose names start with `nbs-<agent>-` and contain `<chat-tag>` are selected. With `--all`, every alive session whose name contains the chat tag is searched.

### Output format

Single agent: `line_num:text`

Multiple agents or `--all`: `name:line_num:text`

### Options

| Option | Description |
|--------|-------------|
| `--from=N` | Start searching at line N (inclusive) |
| `--to=N` | Stop searching at line N (inclusive) |
| `--all` | Search every alive session matching the chat tag |

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | Matches found |
| 1 | No matches (or no alive sessions) |
| 4 | Invalid arguments |

## nbs-ts-query

Extract a range of lines from a single agent session. No pattern matching — just clean text.

### Usage

```
nbs-ts-query <chat-tag> <agent> --from=N --to=N
```

Both `--from` and `--to` are required. `--to` must be >= `--from`.

### What it does

Resolves the session the same way `nbs-ts-grep` does (name starts with `nbs-<agent>-`, contains `<chat-tag>`, process alive). Reads the line range from `output.log`, strips ANSI, and prints `line_num:text` for each non-blank line.

The typical use: grep finds something suspicious at line 4200, query pulls lines 4180-4220 for context.

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Session not found |
| 4 | Invalid arguments |

## How they work

Both tools scan `~/.nbs-ts/sessions/` directories. For each session directory they read the `name` file, check the `pid` file, and call `kill(pid, 0)` to confirm the process is alive. Dead sessions are skipped entirely.

Raw terminal output contains ANSI escape sequences — colours, cursor movement, line clearing. The `strip_ansi` function removes these in place before any matching or printing, so the output is plain text suitable for further processing.

## Who uses them

The medic uses `nbs-ts-grep` for hallucination detection — scanning agent logs for claims that can be checked against reality. Any agent can use either tool when it needs to inspect what another agent has been doing.
