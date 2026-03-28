# Feature Request: Chat File Auto-Repair

## Problem

Occasionally an AI agent writes directly to a chat file in raw text instead of using `nbs-chat send`. This corrupts the file — raw text appears where base64-encoded messages should be. `chat_read` handles this gracefully (skips corrupt lines, increments `skipped_count`) but the content is lost.

Currently, recovery requires a human to manually read the file, identify the corrupt lines, extract the text, and re-send it properly. This is tedious but straightforward.

## Proposal

A bash script `bin/nbs-chat-repair` that:
1. Detects corrupt lines in a chat file
2. Extracts and sanitises recoverable text
3. Blanks the corrupt lines in-place (preserving file byte count)
4. Appends a recovery message with the salvaged content

No file lock required. No `/pause` required. No AI launch required.

## Corruption Taxonomy

Lines after the `---` header separator must be base64-encoded. The following are the ways they can be corrupt:

### Type 1: Raw ASCII text (most common)

An AI writes `@supervisor I fixed the bug` directly to the file. The line contains characters outside the base64 alphabet. Fully recoverable — the text IS the content.

### Type 2: Raw UTF-8 text (multi-byte)

An AI writes text in Chinese, Arabic, Navajo, or any language with non-ASCII characters. The bytes are valid UTF-8 but not base64. Recoverable if UTF-8 validation passes. The repair must not assume ASCII — the recovered text must preserve the original encoding.

### Type 3: Truncated base64

A line that starts as valid base64 but is cut short (length not a multiple of 4). Could happen from a kill during write, power failure, or buffer flush interruption. The decodable prefix may contain partial content. Recovery: attempt to decode the longest valid prefix. If nothing decodes, mark as unrecoverable with a byte count.

### Type 4: Valid base64, invalid wire format

Decodes successfully but the decoded content lacks the `timestamp|handle|content` structure. This is valid base64 wrapping garbage. Rare — would require an AI to base64-encode something and write it to the file without the wire format. Recovery: include the decoded content in the recovery message as-is.

### Type 5: Binary garbage

Non-UTF-8 byte sequences. Filesystem corruption, editor mishap, or binary data written to the file. Not recoverable as text. Recovery: report byte count and hex dump of the first 40 bytes for diagnostic purposes.

### Type 6: Multi-line raw text

An AI writes a multi-line message as raw text. Each line appears as a separate corrupt line in the file:
```
@supervisor I fixed three bugs:
1. The segfault
2. The shutdown crash
```
These are three consecutive corrupt lines that logically belong together. The script should join consecutive corrupt lines into a single recovered message.

Heuristic: consecutive corrupt lines (no valid base64 line between them) are treated as one message. This is imperfect — two unrelated corrupt writes could be adjacent — but it's better than splitting a multi-line message into fragments.

### Type 7: Lines with control characters

Raw text containing ANSI escape sequences, null bytes, or other control characters. These could corrupt the recovery message or the terminal display. Must be stripped before inclusion in the recovery message.

### Type 8: Extremely long lines

A large document pasted directly into the file. Could exceed `MAX_MESSAGE_LEN` (1MB). The recovery message must truncate with an indication of how much was lost.

## Detection

A line after `---` is corrupt if any of:
1. **Length not a multiple of 4** — invalid base64 padding
2. **Contains characters not in `[A-Za-z0-9+/=]`** — not base64 alphabet
3. Both checks are performed on the raw bytes. No decoding is attempted for detection — if either check fails, the line is corrupt.

Type 4 (valid base64, invalid wire format) is not detected by this script. It is valid base64 and will be decoded by `chat_read` normally. The decoded content may be nonsensical but it will not crash anything. This is a deliberate scope limitation — detecting semantic corruption requires understanding the wire format, which is a C-level concern handled by `chat_read`.

## Sanitisation of Recovered Text

Before including recovered text in the recovery message, it must be cleaned:

### UTF-8 validation

Process the recovered bytes left-to-right. For each byte:
- If it starts a valid UTF-8 sequence (1-4 bytes) and the sequence is complete: keep it
- If it starts a multi-byte sequence but the sequence is truncated (end of line): replace with U+FFFD (&#xFFFD;)
- If it is a continuation byte (0x80-0xBF) not preceded by a valid start byte: drop it
- If it is an overlong encoding or a surrogate half: replace with U+FFFD

In bash, `iconv -f UTF-8 -t UTF-8 -c` strips invalid sequences. This is the pragmatic approach — it drops rather than replaces, which loses information but is safe.

### Control character stripping

Remove all bytes 0x00-0x08, 0x0B, 0x0C, 0x0E-0x1F, and 0x7F. Keep:
- 0x09 (tab) — valid in text
- 0x0A (newline) — valid in text
- 0x0D (carriage return) — convert to newline

ANSI escape sequences (`ESC[...m` and similar) should be stripped entirely, not just the ESC byte. A simple approach: `sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'`.

### Length truncation

If the recovered text exceeds 64KB, truncate and append `[truncated — original was N bytes]`. 64KB is well under `MAX_MESSAGE_LEN` (1MB) but large enough for any reasonable message.

## In-Place Replacement

For each corrupt line of N bytes (including trailing newline):

1. Record the byte offset and length in the file
2. Write N-1 space characters (0x20) followed by one newline (0x0A) at the same offset

This preserves:
- **File byte count** — `file-length` header remains valid
- **Line structure** — same number of lines, same positions
- **`chat_read` behaviour** — a line of spaces fails the base64 length check (`len % 4 != 0` for most lengths) and is skipped, just like the original corrupt line was

No file lock is needed because:
- We are writing the same number of bytes to the same offsets — no file growth or shrinkage
- `chat_send` appends to the end — it never reads or writes the interior
- `chat_read` reads the whole file — if it reads during our replacement, it sees either the old corrupt line (skipped) or the new spaces line (skipped). The outcome is identical.

### Edge case: last line without trailing newline

If the last line of the file has no trailing newline, the replacement is N space characters with no newline. The file's byte count is preserved.

### Edge case: line is exactly 4 spaces after replacement

A 4-byte corrupt line becomes 3 spaces + newline. `chat_read` sees a 3-character line, checks `3 % 4 != 0`, skips it. Correct.

### Edge case: line is exactly 1 byte (just newline)

Empty line — `chat_read` already skips empty lines (`len > 0` check). No replacement needed.

## Recovery Message

After blanking corrupt lines, send a recovery message:

```bash
nbs-chat send "$CHAT_FILE" "chat-repair" "$RECOVERY_TEXT"
```

Handle: `chat-repair` — a distinctive name that agents will recognise as automated, not a team member.

Content format:
```
[AUTO-REPAIR] Recovered N corrupt line(s) from chat file.

--- Recovered text (lines M-P) ---
<sanitised content from consecutive corrupt lines>

--- Recovered text (line Q) ---
<sanitised content from isolated corrupt line>

--- Unrecoverable (line R) ---
<byte count and hex dump>
```

If multiple groups of consecutive corrupt lines exist, each group gets its own section with line numbers.

## Script Interface

```
nbs-chat-repair <chat-file> [--dry-run]
```

- Without `--dry-run`: performs the repair and sends the recovery message
- With `--dry-run`: reports what would be done, changes nothing

Exit codes:
- 0 — repair completed (or no corruption found)
- 1 — error (file not found, write failed)
- 4 — invalid arguments

## Trigger: Terminal-Driven Auto-Repair

The repair is triggered automatically by `nbs-chat-terminal`.

`poll_and_display` already calls `chat_read`. After the read, if `state.skipped_count > 0`, the terminal:

1. Shows an INFO line: `INFO> [repair] N corrupt lines detected, running auto-repair...`
2. Runs `nbs-chat-repair <chat-file>` via `spawn_with_capture` — output streams as INFO lines
3. Sets a flag (`g_repair_triggered`) so the repair runs once, not on every poll cycle

The human sees the repair happen in real time via INFO lines. No manual action required.

The repair can also be run manually:
```bash
nbs-chat-repair <chat-file>           # repair now
nbs-chat-repair <chat-file> --dry-run # report only
```

## Index and Cursor Safety Analysis

The in-place blank and recovery append are safe with respect to the cursor system, sidecar notifications, and all concurrent readers/writers.

### How message_count works

`chat_read` walks lines after the `---` header, decoding each. Lines that fail base64 validation are skipped (`skipped_count++`) and NOT counted in `message_count`. Only successfully decoded messages get an index:

```
File lines after ---:
  Line 1: valid base64    → message[0]
  Line 2: CORRUPT         → skipped
  Line 3: valid base64    → message[1]
  Line 4: valid base64    → message[2]

Result: message_count = 3, skipped_count = 1
```

### After blanking the corrupt line

The spaces line still fails base64 validation. `chat_read` skips it, exactly as before:

```
  Line 1: valid base64    → message[0]
  Line 2: SPACES          → skipped (still fails base64 check)
  Line 3: valid base64    → message[1]
  Line 4: valid base64    → message[2]

Result: message_count = 3, skipped_count = 1
```

Message array is identical. All indices unchanged. All cursors valid.

### After appending the recovery message

`nbs-chat send` appends a new line at the end:

```
  Line 1: valid base64    → message[0]
  Line 2: SPACES          → skipped
  Line 3: valid base64    → message[1]
  Line 4: valid base64    → message[2]
  Line 5: recovery msg    → message[3]  ← new

Result: message_count = 4, skipped_count = 1
```

`message_count` goes from 3 to 4. Every sidecar with `cursor=3` sees one new unread message — the recovery message from `chat-repair`. They deliver a notification to their agent. The agent reads the recovery text and continues normally.

### Concurrent access during repair

- **Another `chat_send` during the blank:** `chat_send` appends to the end. It never reads or writes the interior. The blank writes to the interior. These do not conflict. Even without a lock, the writes target different byte ranges.
- **A `chat_read` during the blank:** The reader sees either the old corrupt line (skipped) or the new spaces line (skipped). The outcome is identical — same message array, same indices.
- **A sidecar `chat_poll` during the blank:** `chat_poll` calls `chat_read`. Same analysis — message_count unchanged, cursor comparison unchanged.

### Header consistency

- **`file-length`:** Preserved — the blank writes the same number of bytes. `chat_send` later updates it when appending the recovery message. Both transitions are consistent.
- **`participants`:** The corrupt line's handle was never parsed (skipped), so the participant list is unchanged by the blank. The recovery message adds `chat-repair(1)` via normal `chat_send` header update.
- **`last-writer`:** Updated to `chat-repair` by `chat_send`. Diagnostic only — no agent uses this for logic.

### Auto-archive detection

The terminal checks if `message_count` dropped (indicating archive). The blank does not change `message_count`. The append increases it by 1. Neither triggers the archive detection path.

### Idempotency

Running the repair twice does not cause damage. The first run blanks corrupt lines and appends recovery messages. The second run finds no corrupt lines (space-padded lines are already being skipped as corrupt base64, but they are not recoverable text — they are spaces). The script reports "no corruption found" and exits.

## What Does NOT Change

- `chat_read` — already handles corruption correctly
- `chat_send` — used normally for the recovery message
- The wire format — no changes to encoding/decoding
- The file-length header — preserved by the byte-count-preserving replacement
- Cursor values — the message index array is identical before and after the blank
- Sidecar behaviour — the recovery message appears as a normal new message

## Limitations

- **Type 4 corruption (valid base64, invalid wire format) is not detected.** This is rare and harmless — `chat_read` decodes it and the message appears with garbled content.
- **Multi-line grouping is heuristic.** Consecutive corrupt lines are assumed to be one message. Two unrelated corrupt writes next to each other will be merged. This is acceptable — the alternative (treating each line as separate) splits genuine multi-line messages.
- **The replacement is one-way.** Once corrupt bytes are blanked, the original bytes are gone. The recovered text in the appended message is the only copy. If sanitisation lost information (invalid UTF-8, control characters), that information is gone.
- **No header repair.** If the header itself is corrupt (damaged `file-length`, mangled `participants`), this script does not help. Header corruption requires `nbs-chat-edit` or manual intervention.

## Testing

| Test | Input | Expected |
|------|-------|----------|
| No corruption | Valid chat file | Exit 0, no changes, "no corruption found" |
| Single ASCII line | One raw text line | Line blanked, recovery message appended |
| UTF-8 text | Chinese characters in raw line | Correctly preserved in recovery message |
| Multi-line | 3 consecutive raw lines | Joined into one recovery section |
| Binary garbage | Non-UTF-8 bytes | Hex dump in recovery, bytes blanked |
| Truncated base64 | Length not multiple of 4 | Blanked, partial decode attempted |
| Control characters | ANSI escapes in raw text | Stripped from recovery message |
| Long line | 100KB raw text | Truncated to 64KB with note |
| Dry run | Any corruption | Reports but does not modify |
| Mixed | Valid + corrupt + valid | Only corrupt lines affected |
| Empty file | Just header, no messages | Exit 0, no changes |
| Already repaired | File with space-padded lines | No new damage, no duplicate recovery |
