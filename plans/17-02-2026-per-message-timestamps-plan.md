# Per-Message Timestamps for nbs-chat

## Date: 17-02-2026

## Aim

Add per-message timestamps to nbs-chat so that both humans and AI agents can see when each message was sent. Currently only a single header-level `last-write` timestamp exists.

## Design

### Encoding format change

Current decoded message: `"handle: content"`
New decoded message: `"handle|EPOCH: content"`

Where EPOCH is `time(NULL)` printed as a decimal long (e.g. `1739789287`).

### Backward compatibility

On read, detect old-format messages by checking for `|` before the first `: `. If no `|` exists, set timestamp to 0 (meaning "unknown").

### Display format

`nbs-chat read` displays: `[HH:MM] handle: content` for timestamped messages, `handle: content` for old messages without timestamps.

### Struct change

Add `time_t timestamp` to `chat_message_t`. Value 0 means "no timestamp available".

## Steps

1. **Design** (this document)
2. **Write tests** — unit tests for timestamp round-trip, backward compat, display format
3. **Modify chat_file.h** — add `time_t timestamp` to `chat_message_t`
4. **Modify chat_file.c** — embed timestamp in `chat_send`, parse in `chat_read`
5. **Modify main.c** — display `[HH:MM]` prefix in `cmd_read`
6. **Build and test** — `make clean && make && make test-unit && make test`
7. **Install** — `make install`

## Falsification

- **Round-trip**: send a message, read it back, verify timestamp is within ±2 seconds of `time(NULL)` at send time
- **Backward compat**: manually create a chat file with old-format messages, verify they parse with timestamp=0
- **Display**: verify `[HH:MM]` prefix appears for new messages, no prefix for old messages
- **File-length invariant**: verify file-length header still matches actual file size after timestamp change (the payload is longer now)
