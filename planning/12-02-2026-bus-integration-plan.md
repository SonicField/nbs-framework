# NBS Bus Integration — Plan

**Date:** 12 February 2026
**Terminal goal:** Every `nbs-chat send` automatically publishes a bus event to `.nbs/events/` (if the bus directory exists), enabling event-driven coordination without polling.

## Scope (MVP)

Integration layer only. The core `nbs-bus` binary (publish, check, read, ack, prune, status) is being built by bench-claude. My work adds:

1. **Chat-to-bus bridge in `nbs-chat send`** — after a successful `chat_send()`, publish a `chat-message` event. If the message contains `@handle`, also publish a `chat-mention` event at higher priority.
2. **Test suite for the bridge** — adversarial tests covering the integration.
3. **nbs-poll.md** — already updated by doc-claude. Verify correctness.

Out of scope for MVP: nbs-hub worker-to-bus bridge (depends on full bus), inotifywait, deduplication.

## Falsification Criteria

| Feature | Falsifier |
|---------|-----------|
| Chat-message event | `nbs-chat send` with `.nbs/events/` present → no `.event` file appears |
| Chat-mention event | Message containing `@handle` → no `chat-mention` event file |
| No bus = no crash | `nbs-chat send` without `.nbs/events/` → exits non-zero or crashes |
| Event content | Published event file YAML doesn't match schema (source, type, priority, timestamp, payload) |
| Concurrent safety | 10 parallel `nbs-chat send` → any lost or corrupt event files |
| @ detection | `@handle` at start, middle, end of message — all detected correctly |
| False positive | `email@handle.com` should NOT trigger `chat-mention` (or should it? TBD — check with Alex) |

## Design

### Integration point

`cmd_send()` in `main.c` — after `chat_send()` returns 0, call a new function `bus_publish_chat_event()`.

This keeps the bus logic out of `chat_file.c` (which is the protocol layer) and puts it in the CLI layer where it belongs. The bus is an external coordination feature, not a chat protocol feature.

### Bus event publishing

Two approaches:

**Option A: Shell out to `nbs-bus publish`** — `cmd_send()` calls `nbs-bus publish .nbs/events/ nbs-chat chat-message normal "handle: message"` via `system()` or `execvp()`/`fork()`.

Pros: Simple. No code duplication. If `nbs-bus` changes format, we automatically use the new format.
Cons: Fork+exec overhead. Depends on `nbs-bus` being in PATH.

**Option B: Write event files directly** — `cmd_send()` writes the YAML event file to `.nbs/events/` using the documented format.

Pros: No dependency on nbs-bus binary. No fork overhead. Self-contained.
Cons: Duplicates the event format. Must stay in sync with nbs-bus.

**Decision: Option A (shell out).** The bus binary is the authority on event format. Shelling out keeps a single source of truth. Fork overhead is negligible for a chat send. The dependency on `nbs-bus` in PATH is the same dependency the rest of the system has.

### Bus directory detection

Check for `.nbs/events/` relative to the chat file's directory. The chat file is in `.nbs/chat/`, so the bus directory is `../events/` relative to the chat file, or we resolve via the project root.

Simpler: `nbs-chat send` already has the chat file path. Walk up to find `.nbs/` and check for `events/` within it. If `.nbs/events/` doesn't exist, silently skip — no error, no warning. The bus is opt-in.

### @mention detection

Scan the message for `@word` patterns. A `@` preceded by whitespace or at start of message, followed by `[a-zA-Z0-9_-]+`, is a mention. Extract the handle. This is simple substring matching, not regex — we're in C.

### Implementation sequence

1. Write test suite (tests first)
2. Add `bus_bridge.c` / `bus_bridge.h` — bus event publishing logic
3. Integrate into `cmd_send()` in `main.c`
4. Update Makefile
5. Run test suite + all existing tests
6. Run ASan
7. Document learnings

## Files to modify

| File | Change |
|------|--------|
| `src/nbs-chat/bus_bridge.c` | NEW — bus event publishing functions |
| `src/nbs-chat/bus_bridge.h` | NEW — header for bus bridge |
| `src/nbs-chat/main.c` | Add bus_publish_chat_event() call in cmd_send() |
| `src/nbs-chat/Makefile` | Add bus_bridge.o to LIB_OBJS, add dependency |
| `tests/automated/test_nbs_chat_bus.sh` | NEW — bus integration tests |

## Test plan

### Tests for `test_nbs_chat_bus.sh`

Prerequisite: both `nbs-chat` and `nbs-bus` must be in PATH.

1. **No bus directory — send works normally**: Create chat, send message, verify no `.event` files, verify message in chat.
2. **Bus directory exists — chat-message event created**: Create `.nbs/events/`, send message, verify `*-nbs-chat-chat-message.event` exists.
3. **Event content matches schema**: Read the event file, verify YAML contains source, type, priority, timestamp, payload.
4. **Payload contains handle and message**: Verify the event payload includes the sender handle and message text.
5. **@mention generates chat-mention event**: Send message with `@handle`, verify `*-nbs-chat-chat-mention.event` created with priority `normal` (chat-message) and the chat-mention also created.
6. **Multiple @mentions in one message**: Send `@alice @bob hello`, verify events for both.
7. **No false @mention for email-like strings**: Send message with `user@host.com`, verify no chat-mention event. (TBD — may need to check with Alex.)
8. **Concurrent sends — no lost events**: 10 parallel `nbs-chat send`, verify 10 chat-message events created with unique timestamps.
9. **Bus directory exists but not writable**: Verify send still succeeds (bus failure should not break chat).
10. **nbs-bus binary not in PATH**: Verify send still succeeds (graceful degradation).
11. **Empty message @mention**: Send `@handle`, verify chat-mention event created.
12. **Very long message**: Send 10KB message, verify event created with truncated or full payload.
13. **Special characters in message**: Send message with quotes, newlines, backticks — verify event file is valid YAML.
14. **Event timestamp is reasonable**: Verify timestamp in event file is within 5 seconds of current time.
15. **Bus directory with processed/ subdirectory**: Verify events go to the events directory, not processed/.

### Adversarial tests

16. **Race condition: bus directory deleted between check and write**: Send while `.nbs/events/` is being removed — verify no crash.
17. **Malformed bus config**: Create `.nbs/events/config.yaml` with garbage — verify send still works.
18. **Symlink bus directory**: `.nbs/events/` is a symlink — verify events created correctly.
19. **Full disk simulation**: Fill disk, verify chat send still works (bus event may fail but chat must not).
20. **Binary in message**: Send message with null bytes or binary data — verify no crash.

## Dependencies

- **Blocking on bench-claude**: Tests 2-20 require `nbs-bus` binary for event verification. I can write the tests now but can't run them until bench-claude delivers.
- **Not blocking**: Test 1 (no bus directory) and the bus_bridge code itself can be written and compiled independently.

## Verification

1. `make test` — all existing tests pass (regression)
2. `bash tests/automated/test_nbs_chat_bus.sh` — all bus integration tests pass
3. `make test-asan` — no sanitiser errors
4. Manual test: send messages with and without bus directory, verify correct behaviour
