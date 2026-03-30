# Chat File Format

The `.chat` file is the primary communication channel between agents. It stores a header with metadata followed by base64-encoded messages, one per line.

## Honest Type Definitions

```pascal
type
  { The chat file header occupies the first 6 lines of the file.
    file_length is self-referential: it includes the byte count of
    the file-length header line itself. }
  ChatHeader = record
    magic        : String;    { Always "=== nbs-chat ===" }
    last_writer  : String;    { Handle of the last agent to send }
    last_write   : String;    { ISO 8601 timestamp of last send }
    file_length  : LongInt;   { Total file size in bytes }
    participants : String;    { "handle1(N1), handle2(N2), ..." }
  end;

  { A single participant's message count in the header. }
  Participant = record
    handle : String;          { Max 63 chars (MAX_HANDLE_LEN - 1) }
    count  : LongInt;         { Number of messages sent, >= 0 }
  end;

  { A decoded chat message. Three wire formats exist (see below).
    timestamp is 0 for legacy messages without embedded timestamps,
    or a positive epoch-seconds value for timestamped messages. }
  ChatMessage = record
    timestamp   : LongInt;    { Epoch seconds, 0 = legacy }
    handle      : String;     { Sender handle, max 63 chars }
    content     : String;     { Message text, max 1 MB }
    content_len : LongInt;    { strlen(content), no embedded NULs }
  end;
```

## File Layout

```
=== nbs-chat ===
last-writer: <handle>
last-write: <ISO 8601 timestamp>
file-length: <byte count>
participants: <handle1>(N1), <handle2>(N2), ...
---
<base64 encoded message 1>
<base64 encoded message 2>
...
```

**Line 1:** Magic marker. Exact string `=== nbs-chat ===`.

**Line 2:** `last-writer: ` followed by the handle of the agent that sent the most recent message.

**Line 3:** `last-write: ` followed by an ISO 8601 timestamp (`%Y-%m-%dT%H:%M:%S%z`). Example: `2026-03-30T14:52:17+0000`.

**Line 4:** `file-length: ` followed by the total file size in bytes. This value is self-referential — it includes the byte count of the `file-length:` line itself. The value is computed by an iterative algorithm that accounts for the variable number of digits.

**Line 5:** `participants: ` followed by a comma-separated list of `handle(count)` pairs. Each handle appears exactly once. The count is the number of messages sent by that participant in the current file (recomputed on truncation).

**Line 6:** `---` separator. Marks the end of the header and the start of message data.

**Lines 7+:** One base64-encoded message per line.

## Message Wire Formats

Each message line is base64-decoded to produce a plaintext string in one of three formats. The parser detects the format by the presence of `|` (pipe) separators before the first `: ` (colon-space).

### Format 1: Timestamped with signature (legacy)

```
handle|EPOCH|SIGNATURE: content
```

The `SIGNATURE` field exists from a removed authentication feature. It is parsed but ignored. `EPOCH` is Unix epoch seconds as a decimal integer.

### Format 2: Timestamped (current)

```
handle|EPOCH: content
```

One pipe separator. `EPOCH` is Unix epoch seconds as a decimal integer (int64_t). This is the format used by `chat_send`.

### Format 3: Legacy (no timestamp)

```
handle: content
```

No pipe separator. The timestamp is set to 0 on decode. This format predates the timestamping feature.

All three formats use `: ` (colon followed by space) as the delimiter between the handle/metadata prefix and the message content.

## Atomic Write Protocol

`chat_send` uses a two-phase atomic write:

1. **Write to temporary file:** Create `<path>.tmp`. Write the complete file — updated header, all existing encoded message lines, then the new encoded message line.

2. **Verify postcondition:** `stat(<path>.tmp).st_size` must equal the computed `file-length` header value. If mismatch, abort (assertion failure).

3. **Atomic rename:** `rename(<path>.tmp, <path>)`. This is atomic on POSIX filesystems. The original file is unchanged until the rename succeeds.

4. **On failure:** The `.tmp` file is unlinked. The original file remains intact.

All writes are protected by file locking via `chat_lock_acquire()`, which creates a `.lock` sibling file.

## Limits

| Constant | Value | Description |
|----------|-------|-------------|
| `MAX_HANDLE_LEN` | 64 | Maximum handle length including NUL terminator |
| `MAX_MESSAGE_LEN` | 1,048,576 (1 MB) | Maximum message size |
| `MAX_MESSAGES` | 10,000 | Maximum messages per chat file |
| `MAX_PARTICIPANTS` | 256 | Maximum unique participants |
| `MAX_PATH_LEN` | 4,096 | Maximum file path length |

## See Also

- [Cursor System](cursors.md) — how read positions are tracked
- [Archive and Truncation](archive.md) — what happens when the file grows large
