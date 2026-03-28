# Feature Request: /redraw Command

## Problem

Terminal display occasionally gets corrupted — overlapping text, misaligned prompts, stale content from resize or scroll events. Currently the only fix is to restart the terminal.

## Proposal

A `/redraw` slash command in `nbs-chat-terminal` that clears the screen and repaints the last N messages from the chat file, then redraws the prompt.

## Implementation

```c
if (strcmp(edit.buf, "/redraw") == 0) {
    /* Clear entire screen */
    printf("\033[2J\033[H");

    /* Re-read and display recent messages */
    chat_state_t redraw_state;
    if (chat_read(g_chat_file, &redraw_state) == 0) {
        int start = redraw_state.message_count - 50;
        if (start < 0) start = 0;
        for (int i = start; i < redraw_state.message_count; i++) {
            format_message(redraw_state.messages[i].handle,
                          redraw_state.messages[i].content, g_handle,
                          redraw_state.messages[i].timestamp);
        }
        g_msg_count = redraw_state.message_count;
        chat_state_free(&redraw_state);
    }

    g_cursor_row = 0;
    line_state_reset(&edit);
    print_prompt(g_handle);
    continue;
}
```

## Files

| File | Change |
|------|--------|
| `src/nbs-chat/terminal.c` | Add `/redraw` command handler |

## What Does NOT Change

- Chat file — read only
- Message count tracking — reset to current count
- Filter state — respected (if `/filter` is active, redraw shows filtered view)

## Verification

1. Type `/redraw` — screen clears, last 50 messages reappear, prompt redraws
2. Resize terminal, type `/redraw` — display corrects
3. With `/filter scribe` active, `/redraw` shows only scribe's messages
4. `/help` lists `/redraw`
