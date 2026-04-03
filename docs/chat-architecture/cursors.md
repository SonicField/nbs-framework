# Cursor System

The cursor system tracks each agent's read position in a chat file. Sidecars use cursors to determine which messages are unread and whether to send a notification.

## Honest Type Definition

```pascal
type
  { A single cursor entry: the last-read message index for one agent.
    position is 0-based. A value of -1 means no cursor exists
    (agent has not read the file yet — treat as "start from beginning").
    A value of -2 means the cursor entry exists but failed to parse
    (file corruption). }
  CursorEntry = record
    handle   : String;    { Agent handle, max 63 chars }
    position : LongInt;   { 0-based last-read message index }
  end;

  { The complete cursor file: one entry per agent that has read
    or sent to the chat file. }
  CursorFile = sequence of CursorEntry;
```

## File Format

The cursor file lives at `<chat-path>.cursors`. For example, if the chat file is `/project/.nbs/chat/team.chat`, the cursor file is `/project/.nbs/chat/team.chat.cursors`.

```
# Read cursors — last-read message index per handle
alice=125
bob=87
charlie=200
```

**Line 1:** Comment header (preserved on every write).

**Subsequent lines:** `handle=index` pairs, one per line. The handle is the agent's unique identifier. The index is a non-negative integer representing the 0-based position of the last message the agent has read.

Blank lines and lines starting with `#` are skipped on read.

## Operations

### Reading a Cursor

`chat_cursor_read(chat_path, handle)` returns:
- `>= 0` — the last-read message index for the handle
- `-1` — no cursor exists (file missing or handle not found)
- `-2` — cursor entry exists but value failed to parse (corruption)

### Writing a Cursor

`chat_cursor_write(chat_path, handle, index)` atomically updates the cursor:

1. Acquires a lock on the main chat file.
2. Reads all existing cursors into memory.
3. Updates or adds the entry for the given handle.
4. Writes to `<cursors-path>.tmp`, then renames atomically.
5. Releases the lock.

The lock is held throughout to prevent concurrent corruption from multiple sidecars updating the same cursor file.

## Ownership and Semantics

**What the cursor means:** A cursor value of N means "messages 0 through N have been *delivered to the agent's terminal*." It does NOT mean the agent has read, understood, or acted on those messages. The cursor tracks delivery, not comprehension — analogous to email read receipts.

**Two writers, one contract:** Both the sidecar and the agent (via `nbs-chat read --unread=<handle>`) can advance the cursor. Both use `chat_cursor_write()` which acquires the chat lock, so there is no data corruption from concurrent writes. The semantic contract is:

- **Sidecar** does not advance the cursor directly. It reads the cursor to compute unread counts and deliver notifications.
- **Agent** advances the cursor via `--unread` after messages are displayed in the terminal.
- **`chat_send`** advances the sender's cursor automatically (see Sender Auto-Advance below).

**Monotonicity:** Cursors only move forward (except during archive adjustment or explicit reset via `nbs-chat cursor-set`). A cursor value of N guarantees that on the next poll cycle, the sidecar will compute `unread = total - N - 1`. Any new message (total increases by 1) makes `unread > 0`, which triggers a notification. This invariant ensures that agent `--unread` advancement does not prevent the sidecar from notifying on subsequent messages.

**CLI tools for cursor management:**
- `nbs-chat cursor-set <file> <handle> <value>` — atomically set a cursor (lock-safe). Use this instead of `sed -i` which bypasses the lock.
- `nbs-chat count <file>` — authoritative message count using separator-based counting. Use this instead of `wc -l - 6` which assumes a fixed header size.

## Sender Auto-Advance

When an agent sends a message via `chat_send`, the sender's cursor is automatically advanced to the index of the newly appended message. This happens *after* the lock is released on the main chat file.

If auto-archiving occurred during the send (see [Archive and Truncation](archive.md)), the cursor index is adjusted: `index = encoded_line_count - ARCHIVE_CLEAVE`.

## Unread Calculation

The number of unread messages for an agent is:

```
unread = total_message_count - cursor_position - 1
```

If the cursor does not exist (return -1), all messages are unread.

## Cursor Adjustment During Archiving

When `chat_auto_archive` moves the first 1,000 messages to an archive file, all cursors are decremented by the cleave count (1,000):

```
new_cursor = old_cursor - ARCHIVE_CLEAVE
if new_cursor < 0 then new_cursor = 0
```

An agent whose cursor pointed at message 500 (now archived) gets cursor 0. An agent whose cursor pointed at message 1,500 gets cursor 500 — correctly pointing to the same logical message in the shortened file.

The cursor adjustment is atomic: the updated cursor file is written to `.tmp` and renamed.

## Desync and Repair

Cursors can desync if:
- An agent crashes between reading messages and updating its cursor
- The cursor file is corrupted (returns -2)
- An archive operation races with a cursor write

The `nbs-fixup` periodic trigger detects and repairs cursor inconsistencies. To reset a cursor, use `nbs-chat cursor-set <file> <handle> <value>`. Set to `msg_count - 1` (via `nbs-chat count`) so the agent sees at least the most recent message on restart. Never use `sed -i` on cursor files — it bypasses the chat lock and races with concurrent cursor writers.

## See Also

- [Chat File Format](chat-file.md) — the file whose read positions are tracked
- [Archive and Truncation](archive.md) — how archiving adjusts cursors
- [Sidecar Notifications](sidecar.md) — how the sidecar uses cursors to detect unreads
