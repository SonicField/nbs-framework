# nbs-scribe-log: Decision Log Tool

A deterministic CLI for appending structured decision entries to the scribe log. Replaces the previous approach of having the LLM construct heredocs and pipe them through the bus — which was fragile, format-inconsistent, and vulnerable to injection.

## Why a Binary

The scribe log format is precise: timestamped headings, specific field names, consistent ordering. An LLM generating this via shell heredocs will eventually get it wrong — a missing field, a swapped label, a newline in the summary that creates a fake entry. `nbs-scribe-log` eliminates this class of error. The tool validates inputs, rejects newlines (markdown injection), generates the timestamp, formats the entry, appends under a file lock, and publishes a bus event. The LLM's job is reduced to deciding *what* to record, not *how*.

## Usage

```
nbs-scribe-log <log-file> <summary> [options]
```

### Required

| Option | Description |
|--------|-------------|
| `<log-file>` | Path to the scribe log (`.md`) |
| `<summary>` | One-line decision summary |
| `--participants=<a,b,c>` | Comma-separated participant handles |
| `--rationale=<text>` | 1-3 sentence rationale |

### Optional

| Option | Default | Description |
|--------|---------|-------------|
| `--chat-ref=<file:~Lnnn>` | — | Chat file and approximate line |
| `--artefacts=<paths>` | — | Commit hashes, file paths, or `-` |
| `--risk-tags=<tags>` | — | Comma-separated tags, or `none` |
| `--status=<status>` | `decided` | One of: `decided`, `superseded`, `reversed`, `mitigated` |
| `--supersedes=<D-timestamp>` | — | Link to the decision being superseded |
| `--bus-dir=<path>` | `.nbs/events/` | Bus directory for event publication |

## Operations

**Append.** The only write operation. Generates a `D-<unix-timestamp>` entry, validates all fields, appends to the log file under an exclusive file lock, and publishes a `scribe decision-logged` bus event. Prints the decision ID to stdout on success.

**Query.** Not provided by this tool — the log is plain markdown. Use `grep "^### D-"` for listing, or the queries documented in [nbs-scribe](../team/nbs-scribe.md).

**Compact.** Not provided. The log is append-only by design. Status changes are new entries, not edits.

## Security

All string fields are rejected if they contain newlines. A newline in a summary or rationale could inject a fake `### D-` heading, creating a spurious decision entry in the log. The tool treats this as a hard error, not a sanitisation opportunity.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success (decision ID printed to stdout) |
| 1 | I/O or lock error |
| 4 | Invalid arguments |

## See Also

- [nbs-scribe](../team/nbs-scribe.md) — the scribe role and log format specification
- [nbs-bus](nbs-bus.md) — event publication on decision logging
