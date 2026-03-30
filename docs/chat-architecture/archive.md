# Archive and Truncation

When a chat file grows beyond a threshold, auto-archiving moves the oldest messages to a separate archive file. This keeps the main chat file manageable while preserving history.

## Honest Type Definitions

```pascal
type
  { Archive configuration constants.
    These are compile-time constants in chat_file.c. }
  ArchiveConfig = record
    threshold : LongInt;  { 2000 — trigger when message count exceeds this }
    cleave    : LongInt;  { 1000 — number of messages moved to archive }
  end;

  { An archive file preserves the standard chat file format
    (header + base64 messages) for the oldest messages. }
  ArchiveFile = record
    filename  : String;   { "<basename>-YYYYMMDD-HHMMSS-archive.chat" }
    messages  : sequence of ChatMessage;
  end;
```

## Auto-Archive Trigger

Auto-archiving is triggered inside `chat_send`, after the new message has been successfully written, while the file lock is still held:

```
if total_message_count > ARCHIVE_THRESHOLD (2000):
    archive first ARCHIVE_CLEAVE (1000) messages
    keep remaining ~1000 messages in main file
```

The check uses the post-send message count (existing messages + 1 new message).

## Archive File Naming

```
<path-without-extension>-<YYYYMMDD>-<HHMMSS>-archive.chat
```

Example: if the chat file is `/project/.nbs/chat/team.chat` and archiving happens at 2026-03-30 14:52:17, the archive file is:

```
/project/.nbs/chat/team-20260330-145217-archive.chat
```

The timestamp uses `strftime` format `"%Y%m%d-%H%M%S"`.

## Archive Process

1. **Read the current file** — parse all messages.

2. **Split messages** — first `ARCHIVE_CLEAVE` (1,000) messages go to the archive, remaining messages stay in the main file.

3. **Write archive file** — standard chat file format (header + base64 messages). The header's `participants` field is recomputed from only the archived messages. Written atomically (temp + rename).

4. **Rewrite main file** — same atomic write protocol as `chat_send`. The header is recomputed for the remaining messages. The `participants` counts reflect only the messages that remain.

5. **Adjust cursors** — all cursor entries in the `.cursors` file are decremented by `ARCHIVE_CLEAVE`:

```
new_cursor = old_cursor - 1000
if new_cursor < 0: new_cursor = 0
```

The cursor adjustment is written atomically (temp + rename) inside the same lock scope.

Both the archive file write and the main file rewrite use `ASSERT_MSG` to verify that the written file size matches the computed `file-length` header value.

## Terminal Archive Detection

The terminal (`terminal.c`) tracks the current message count in a global variable. When it detects that the message count has dropped between polls, it knows an archive event has occurred:

```c
if (state.message_count < g_msg_count) {
    // Archive detected — reset counter to resume polling
}
```

This prevents the terminal from going "deaf" after an archive — without this check, it would think it had already read all messages up to the old count.

## Cursor Survival

After archiving:

- An agent whose cursor was at message 500 (now in the archive) gets cursor 0. The agent will re-read from the beginning of the shortened file — but those messages were at positions 1000-2000 in the original file, so the agent only sees messages it hadn't read yet (messages 1000+).

- An agent whose cursor was at message 1,500 gets cursor 500. This correctly points to the same logical message in the shortened file.

- The sender's cursor is also adjusted. `chat_send` computes the post-archive cursor index as `encoded_line_count - ARCHIVE_CLEAVE`.

## Registry Exclusion

The sidecar's `registry_seed` function skips archive files when scanning `.nbs/chat/` for chat files to monitor. Files containing `-archive.` in their name are excluded. This prevents the sidecar from monitoring archive files that are not actively written to.

## Truncation Without Archiving

`chat_truncate(path, keep_count)` directly truncates a chat file to `keep_count` messages without creating an archive. It rewrites the file atomically with updated headers. This function is used for manual truncation, not auto-archiving.

If `keep_count >= message_count`, it is a no-op.

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `ARCHIVE_THRESHOLD` | 2,000 | Message count that triggers auto-archive |
| `ARCHIVE_CLEAVE` | 1,000 | Messages moved to archive per trigger |

## See Also

- [Chat File Format](chat-file.md) — the file format for both main and archive files
- [Cursor System](cursors.md) — how cursors are adjusted during archiving
