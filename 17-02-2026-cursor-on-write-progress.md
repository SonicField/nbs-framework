# Cursor-on-Write Fix Progress

**Date**: 17-02-2026

## Completed

### Step 1: Tests written (T21a-T21d)
- T21a: Single send updates cursor to message index 0
- T21b: After send, `--unread` returns no messages for sender
- T21c: Two senders have independent cursors at their last message index
- T21d: Sequential sends update cursor each time

All 4 tests failed before the fix (cursor returned -1). Confirmed failing state.

### Step 2: Implementation
- Modified `chat_file.c:chat_send()` — added `chat_cursor_write(path, handle, encoded_line_count)` after lock release and cleanup
- The call is placed after `chat_lock_release()` to avoid deadlock (fcntl locks are per-process but the code paths use separate fd opens)
- Cursor update failure is non-fatal — logged as warning, send still returns 0
- New message index = `encoded_line_count` (0-based count of existing messages before append)

### Step 3: Test updates
Three integration tests (T2 test 12, T7 test 33) needed updating to reflect the new cursor-on-write semantics:
- Test 12: Cursor file now has 4 entries (2 senders + 2 readers), not 2
- Test 33: handleX/handleZ `--unread` now returns fewer messages (their cursor was advanced by cursor-on-write)
- Test comments updated to explain the cursor-on-write behaviour

### Step 4: Verification
- All 19 chat_file unit tests pass (15 existing + 4 new T21)
- All 89 unit tests pass across 6 suites
- All lifecycle tests pass (19 assertions)
- All bus integration tests pass (20 tests)
- All terminal tests pass (37 assertions)
- All integration tests pass (117 tests, 236 assertions)

## Learnings

1. The change is minimal (12 lines of C + 1 warning line) but has visible impact on existing tests. The integration test assumptions about cursor state were implicitly documenting the old behaviour (send doesn't update cursors). Updating these tests forced me to articulate the new semantics explicitly.

2. The race window between lock release and cursor write is benign: the worst case is another message arriving in between, making our cursor one position behind. The next send or `--unread` read corrects this.

3. fcntl advisory locks are per-process, not per-fd. Two separate `open()` calls to the same lock file from the same process will both succeed with F_SETLKW. This means calling `chat_cursor_write` after `chat_lock_release` in `chat_send` is safe — there's no deadlock risk, just a brief window without the lock.

## Files Changed
- `src/nbs-chat/chat_file.c` — cursor-on-write in `chat_send()`
- `tests/test_chat_file_unit.c` — T21a-T21d tests
- `tests/automated/test_integration.sh` — updated T2/T7 to match new semantics
