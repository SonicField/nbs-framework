# Cursor-on-Write Fix Plan

**Date**: 17-02-2026
**Task**: nbs-chat send should update sender's read cursor to position of written message
**Reference**: Option 2 from team discussion; Pythia #1 integration gap

## Problem

When an agent calls `nbs-chat send`, the sender's read cursor is not updated. This causes:

1. **Self-notification**: The sidecar detects the sender's own message as "unread" and generates a spurious notification event.
2. **Cursor desync on restart**: After cursor cleanup (Step 5b), cursors are reset to tail. But if the chat grows further, a stale cursor can point past the end of the file, causing the assertion crash: `start=1250 end=1246 message_count=1246`.
3. **Workaround pollution**: Agents currently do a manual `nbs-chat read --unread=<handle>` after every send — adding latency and complexity.

## Design

**Change**: In `chat_send()` (chat_file.c), after successfully writing the message, call `chat_cursor_write()` to set the sender's cursor to the index of the newly written message (i.e., `new_message_count - 1`).

**Location**: `chat_file.c:chat_send()`, between the successful write verification and the cleanup section (around line 562).

**Key consideration**: `chat_send` already holds the lock. `chat_cursor_write` also acquires the lock. This would deadlock with `fcntl` advisory locks. Two approaches:
- (a) Extract cursor write logic into an internal function that assumes lock is held
- (b) Release the chat lock before calling cursor_write, then let cursor_write re-acquire it

Option (a) is cleaner — no window where another writer could interleave between the send and cursor update. However, `chat_cursor_write` uses the same lock file as `chat_send` (confirmed in test 11), and `fcntl` locks are per-process, not per-fd. So the same process re-acquiring the same lock should succeed (fcntl locks are re-entrant within a process).

**Verification needed**: Check if fcntl F_SETLKW is re-entrant for the same process.

Actually, looking at the code more carefully: `chat_send` releases the lock (line 569) before returning. If we insert the cursor update BEFORE the lock release, we need the internal function approach. If we insert it AFTER the lock release, `chat_cursor_write` can re-acquire normally.

**Decision**: Place the cursor update AFTER `chat_lock_release` but BEFORE the cleanup/return. This avoids the deadlock and keeps the code simple. The race window (another process writing between send and cursor update) is acceptable — the cursor will point to the message we just wrote or a later one, which is correct either way.

Wait — re-reading the code. `chat_send` calls `chat_lock_release(lock_fd)` at line 569. But `chat_cursor_write` also calls `chat_lock_acquire`. With fcntl advisory locks, a process can re-lock the same file because fcntl locks are per-process (not per-fd). So calling `chat_cursor_write` after `chat_lock_release` is safe — no deadlock.

But actually simpler: we can just call `chat_cursor_write` after `chat_lock_release` in `chat_send`. The cursor update doesn't need to be atomic with the send. The worst case: another message arrives between send and cursor update, making our cursor one position behind. Next send will fix it.

**Final design**: After the lock release and cleanup in `chat_send`, call `chat_cursor_write(path, handle, new_message_index)`. The new_message_index is `encoded_line_count` (the 0-based index of the new message = count of existing messages before we added ours).

## Falsification

**What would prove the fix wrong:**
1. Sending a message should result in the sender's cursor pointing to that message's index
2. After sending, `--unread=<sender>` should return no messages (since we just wrote the latest)
3. After another agent sends, `--unread=<first-sender>` should return only the new message
4. Concurrent sends from two handles should not corrupt each other's cursors
5. The fix should not change the return value or error behaviour of `chat_send`

## Testable Steps

### Step 1: Write tests (T21 — cursor-on-write)
Add to `test_chat_file_unit.c`:
- T21a: `chat_send` updates sender cursor — after send, `chat_cursor_read` returns the sent message's index
- T21b: After send, `--unread` (simulated via cursor read + message count) shows no unread for sender
- T21c: After two different senders, each sender's cursor is at their last message's index
- T21d: Sending multiple messages updates cursor each time to latest

### Step 2: Implement the fix
Modify `chat_send()` in `chat_file.c` to call `chat_cursor_write` after successful send.

### Step 3: Build and verify
- `make clean && make` in `src/nbs-chat/`
- Run unit tests: `make test-unit`
- Run full test suite: `make test-all`
- Install: `make install`

### Step 4: Integration test with live chat
Verify the fix resolves the `--unread` crash by testing against a real chat file.
