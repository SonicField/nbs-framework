# nbs-chat-repair

Detect and repair corrupt lines in nbs-chat files. Blanks corrupt lines in-place (preserving file byte count) and appends a recovery message with the salvaged content.

## Usage

```bash
nbs-chat-repair <chat-file>           # repair now
nbs-chat-repair <chat-file> --dry-run # report only, change nothing
```

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Repair completed, or no corruption found |
| 1 | Error (file not found, write failed) |
| 4 | Invalid arguments |

## How It Works

1. **Detect** — scans lines after the `---` header separator. A line is corrupt if its length is not a multiple of 4 or it contains characters outside the base64 alphabet (`[A-Za-z0-9+/=]`).

2. **Group** — consecutive corrupt lines are treated as one message (heuristic: a multi-line raw text paste).

3. **Sanitise** — recovered text is cleaned:
   - Invalid UTF-8 sequences stripped (`iconv -c`)
   - ANSI escape sequences removed
   - Control characters (0x00–0x08, 0x0B, 0x0C, 0x0E–0x1F, 0x7F) stripped; tab and newline kept; CR converted to newline
   - Content exceeding 64KB truncated with a note

4. **Blank** — each corrupt line is overwritten with spaces of the same byte length, preserving file byte count and line structure. `chat_read` skips these lines identically to the original corruption.

5. **Recover** — a recovery message is appended via `nbs-chat send` with handle `chat-repair`, containing the sanitised content grouped by line numbers.

## Corruption Types Handled

| Type | Example | Recovery |
|------|---------|----------|
| Raw ASCII text | `@supervisor I fixed the bug` | Full text preserved |
| Raw UTF-8 text | `这是中文消息` | UTF-8 preserved |
| Multi-line raw text | 3 consecutive raw lines | Joined into one recovery section |
| Truncated base64 | Length not multiple of 4 | Blanked, noted in recovery |
| Control characters | ANSI escapes, null bytes | Stripped before recovery |
| Long lines (>64KB) | Large document paste | Truncated to 64KB with note |
| Binary garbage | Non-UTF-8 bytes | Hex dump of first 40 bytes |

**Not handled:** Type 4 (valid base64, invalid wire format) — rare and harmless, handled by `chat_read` normally.

## Safety Properties

- **File byte count preserved** — the `file-length` header remains valid after blanking
- **Message indices unchanged** — `chat_read` produces the same message array before and after blanking (corrupt lines were already skipped)
- **Cursor-safe** — all cursor values remain valid; the recovery message appears as a normal new message
- **Concurrent-safe** — no file lock needed; blanking writes to interior bytes while `chat_send` appends to the end
- **Idempotent** — running twice causes no damage; space-padded lines from prior repairs are skipped

## Limitations

- The original corrupt bytes are gone after blanking — the recovery message is the only copy
- Multi-line grouping is heuristic — two unrelated corrupt writes on adjacent lines will be merged
- Header corruption (damaged `file-length`, mangled `participants`) is not repaired — use `nbs-chat-edit` for that

## Terminal Integration

The terminal (`nbs-chat-terminal`) can trigger auto-repair when `chat_read` reports `skipped_count > 0`. This integration is planned — see the feature request for details. Manual invocation works now.
