# Feature Request: Browse Mode and Shared Chat View Library

## Problem

The terminal (`nbs-chat-terminal`) displays chat as a scrolling log — new messages appear at the bottom, old messages scroll off the top. There is no way to scroll back, search, or navigate the conversation history without leaving the terminal.

`nbs-chat-edit` provides exactly this: a full-screen TUI with vim-style navigation, search (`/`, `n`, `N`), and message viewing (`Enter`/`v`). But launching `nbs-chat-edit` from outside the terminal is clumsy — it requires knowing the chat file path — and it includes editing features (delete, truncate, write) that are unnecessary and dangerous for casual browsing.

The two tools duplicate rendering logic. The editor has its own `render()` function in `editor.c` that handles message list layout, cursor movement, scroll position, search highlighting, and content preview. The terminal has `format_message()` in `render.c` for inline message rendering. Both call `handle_colour_str()` and `handle_style_lookup()` from shared code, but the TUI navigation and layout code is not shared.

## Proposal

### 1. Extract a shared chat view library (`libchatview`)

Extract the scrollable message list TUI into a shared library that both `nbs-chat-edit` and `nbs-chat-terminal` can link against.

The library provides:

- **Message list rendering** — full-screen layout with header, scrollable message area, status bar
- **Navigation** — cursor movement (j/k, arrows, Page Up/Down, Home/End, G/g)
- **Search** — forward search (`/`), next/previous match (`n`/`N`), regex support
- **Full message view** — expand a single message to fill the screen (`Enter`/`v`)
- **Handle colouring** — uses existing `handle_colour_str()` and `handle_style_lookup()`
- **Mention highlighting** — uses existing `write_content_highlighted()` when active
- **Read-only by default** — the library renders and navigates but does not modify the chat file

The library does NOT provide editing operations (delete, truncate, write). Those remain in `nbs-chat-edit` as a thin layer on top of the view library.

### 2. `/browse` command in the terminal

The terminal gains a `/browse` command:

```
/browse          Open scrollable view of full chat history
/browse <pattern> Open scrollable view, jump to first match
```

Behaviour:

1. The terminal saves its current state (input buffer, cursor position)
2. The terminal enters browse mode — the view library takes over the full screen
3. The user navigates with the same keys as `nbs-chat-edit` (j/k, /, n, G, etc.)
4. Pressing `q` or `Escape` exits browse mode
5. The terminal restores its state and redraws the normal chat view

While in browse mode:
- New messages continue to arrive (the poll loop runs in the background)
- A status bar indicator shows the count of new messages since browse started
- Pressing `G` (go to end) shows the latest messages including any that arrived during browse
- The user cannot send messages — all input goes to the view library

### 3. Refactor `nbs-chat-edit`

After the library extraction, `nbs-chat-edit` becomes a thin wrapper:

1. Initialise the view library with the chat file
2. Add editing keybindings on top (d, t, u, Ctrl-R, w)
3. Add the write-back logic (backup, recreate, re-send)
4. Add the dirty/modified state tracking

The existing editing functionality is unchanged from the user's perspective.

## Source Layout

```
src/nbs-chatview/
    chatview.c       — shared TUI: layout, navigation, search, message view
    chatview.h       — public API
    Makefile         — builds libchatview.a

src/nbs-chat-edit/
    editor.c         — editing layer on top of libchatview
    Makefile         — links libchatview.a

src/nbs-chat/
    terminal.c       — /browse command invokes chatview for read-only browsing
    render.c         — inline message rendering (unchanged, used for the scrolling log)
    Makefile         — links libchatview.a
```

## Library API (sketch)

```c
/* Initialise a chat view from a loaded chat state */
chatview_t *chatview_init(const chat_state_t *state, const char *title);

/* Run the view event loop — blocks until the user exits (q/Escape) */
/* Returns the number of the message the cursor was on when exiting */
int chatview_run(chatview_t *cv);

/* Update the view with new messages (call from poll loop) */
void chatview_update(chatview_t *cv, const chat_state_t *new_state);

/* Set an initial search pattern (for /browse <pattern>) */
void chatview_search(chatview_t *cv, const char *pattern);

/* Register a callback for editing keybindings (nbs-chat-edit only) */
typedef int (*chatview_key_handler_t)(chatview_t *cv, int key, void *userdata);
void chatview_set_key_handler(chatview_t *cv, chatview_key_handler_t handler, void *userdata);

/* Clean up */
void chatview_free(chatview_t *cv);
```

The key handler callback lets `nbs-chat-edit` intercept `d`, `t`, `u`, `w` etc. without the library knowing about editing. Keys not handled by the callback fall through to the default navigation.

## What Does NOT Change

- The terminal's normal scrolling chat display — `/browse` is a mode you enter and exit
- The chat file format
- `nbs-chat read` / `nbs-chat send` — browse is read-only, uses `chat_read` directly
- Cursor system, sidecar behaviour — browse is terminal-local

## Testing

| Test | Verification |
|------|-------------|
| `/browse` opens and `q` exits | Terminal state fully restored after exit |
| Navigation in browse mode | j/k/arrows move cursor, Page Up/Down scroll, G/g jump to end/start |
| Search in browse mode | `/pattern` highlights matches, `n`/`N` cycle through them |
| New messages during browse | Status bar shows count, `G` reveals them |
| `/browse <pattern>` | Opens with cursor on first match |
| `nbs-chat-edit` unchanged behaviour | All existing edit tests pass after refactor |
| Library builds independently | `make` in `src/nbs-chatview/` produces `libchatview.a` |
