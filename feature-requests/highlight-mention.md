# Feature Request: --highlight-mention for nbs-chat-terminal

## Problem

In a busy team chat, messages directed at the human (`@alex`, `@martin`) are visually identical to messages directed at other agents (`@supervisor`, `@theologian`). The human must read every message to find the ones addressed to them.

## Proposal

A `--highlight-mention` flag on `nbs-chat-terminal` that inverts text matching `@<handle>` in chat messages, where `<handle>` is the human's handle (the second positional argument).

```
nbs-chat-terminal .nbs/chat/live.chat martin --highlight-mention
```

Any occurrence of `@martin` in a chat message renders with inverse video (RENDER_REVERSE / `\033[7m`). The `martin>` prompt also renders inverted, giving visual continuity.

## Matching

The match must be `@handle` followed by a word boundary — not a prefix match. `@martin` matches but `@martinque` does not.

Word boundary: the character after the handle is not alphanumeric, underscore, or hyphen — or is end of string. This matches the same rule used by `bus_bridge.c` for @mention extraction.

```c
/* After matching @handle, check the next character */
char next = text[pos + handle_len];
int is_boundary = (next == '\0' || (!isalnum(next) && next != '_' && next != '-'));
```

This is a UI feature. False positives on edge cases (e.g. `@martin.` matching) are acceptable — the consequence is a highlighted word that didn't need highlighting, not a system failure.

## Changes

| File | Change |
|------|--------|
| `src/nbs-chat/terminal.c` | Parse `--highlight-mention` flag, store in global. Pass to `format_message`. |
| `src/nbs-chat/render.h` | Add `render_set_highlight_handle(const char *handle)` |
| `src/nbs-chat/render.c` | In `render_message`: scan content for `@handle` at word boundaries, wrap matches in `\033[7m...\033[0m`. In `render_message_own`: same treatment. |
| `src/nbs-chat/terminal.c` | `print_prompt`: if highlight enabled, render `handle>` with inverse. |

## What Does NOT Change

- `render_message_medic` — medic warnings are already visually distinct
- Chat file format — this is rendering only
- The @mention extraction in `bus_bridge.c` — that's for bus events, this is for display
- `nbs-chat export` — could be added later but not in scope

## Verification

1. Launch with `--highlight-mention`, send `@martin hello` from another agent — `@martin` renders inverted
2. `@martinque` does NOT render inverted
3. `@martin!` and `@martin?` and `@martin,` all render inverted (mention + punctuation)
4. Multiple `@martin` in one message — all inverted
5. `@martin` at start, middle, and end of message — all work
6. Prompt `martin>` renders inverted
7. Launch without `--highlight-mention` — no change in behaviour
