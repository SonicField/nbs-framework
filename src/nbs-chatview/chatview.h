/*
 * chatview.h — Shared scrollable chat view TUI library
 *
 * Provides a full-screen message list with navigation, search, and
 * message viewing. Used by both nbs-chat-terminal (/browse command)
 * and nbs-chat-edit (with editing extensions via key handler callback).
 *
 * Read-only by default: the library renders and navigates but does
 * not modify the chat file. Editing operations are layered on top
 * by consumers via chatview_set_key_handler().
 */

#ifndef NBS_CHATVIEW_H
#define NBS_CHATVIEW_H

#include "../nbs-chat/chat_file.h"
#include "../nbs-chat/render.h"
#include <regex.h>

/* --- Per-message flags (set by consumers) --- */

#define CHATVIEW_MSG_DELETED    (1 << 0)

/* --- Key handler return values --- */

#define CHATVIEW_KEY_UNHANDLED  0   /* Default handler should process */
#define CHATVIEW_KEY_HANDLED    1   /* Key was consumed */
#define CHATVIEW_KEY_QUIT       2   /* Request exit from the view */

/* --- Key codes (shared between library and consumers) --- */

enum chatview_key {
    CHATVIEW_KEY_NONE = 0,
    CHATVIEW_KEY_UP = 256,
    CHATVIEW_KEY_DOWN,
    CHATVIEW_KEY_LEFT,
    CHATVIEW_KEY_RIGHT,
    CHATVIEW_KEY_PAGE_UP,
    CHATVIEW_KEY_PAGE_DOWN,
    CHATVIEW_KEY_HOME,
    CHATVIEW_KEY_END,
    CHATVIEW_KEY_CTRL_R = 18,
};

/* --- Chatview state (public — consumers may read fields directly) --- */

typedef struct chatview chatview_t;

/* Key handler callback: return CHATVIEW_KEY_HANDLED, _UNHANDLED, or _QUIT */
typedef int (*chatview_key_handler_t)(chatview_t *cv, int key, void *userdata);

/* Poll callback: called periodically during the event loop */
typedef void (*chatview_poll_fn)(chatview_t *cv, void *userdata);

struct chatview {
    /* Chat state — owned by chatview, freed on free/update */
    chat_state_t state;
    char *title;

    /* View state */
    int cursor;         /* current message index */
    int scroll_top;     /* first visible message */
    int dirty;          /* "[modified]" indicator (set by consumer) */
    char status[256];   /* status bar message */

    /* Search */
    char search[256];   /* current search pattern */
    regex_t search_re;  /* compiled regex */
    int search_valid;   /* regex compiled successfully */

    /* Per-message flags (calloc'd, resized on update) */
    uint8_t *msg_flags;
    int msg_flags_count;

    /* Render state */
    int needs_redraw;   /* 1 = screen must be repainted */

    /* Terminal dimensions */
    int term_rows;
    int term_cols;

    /* New message tracking (for /browse mode) */
    int initial_count;  /* message count at init time */

    /* Extension callbacks */
    chatview_key_handler_t key_handler;
    void *key_handler_data;
    chatview_poll_fn poll_fn;
    void *poll_data;

    /* Help text override (NULL = use default) */
    const char *help_hint;
};

/* --- Lifecycle --- */

/*
 * chatview_init — Create a chat view from a loaded chat state.
 *
 * Takes ownership of the state's data (caller must not free it).
 * The title string is copied.
 *
 * Returns NULL on allocation failure.
 */
chatview_t *chatview_init(const chat_state_t *state, const char *title);

/*
 * chatview_free — Release all resources.
 *
 * Frees the chat state, msg_flags, title, and the chatview itself.
 */
void chatview_free(chatview_t *cv);

/* --- Event loop --- */

/*
 * chatview_run — Run the view event loop.
 *
 * Enters alternate screen, enables raw mode. Blocks until the user
 * exits (q or Escape — unless overridden by key handler).
 *
 * Returns the cursor position (message index) at exit time.
 */
int chatview_run(chatview_t *cv);

/* --- State updates --- */

/*
 * chatview_update — Replace chat state with new data.
 *
 * Call from poll callback to add new messages. Frees the old state.
 * Preserves cursor position and search state. Grows msg_flags for
 * new messages (new entries get flags=0).
 */
void chatview_update(chatview_t *cv, const chat_state_t *new_state);

/*
 * chatview_reload — Re-read a chat file and update the view.
 *
 * Convenience wrapper: reads the file, calls chatview_update().
 * Reallocates msg_flags to match new message count.
 * Returns 0 on success, -1 on read failure.
 */
int chatview_reload(chatview_t *cv, const char *path);

/* --- Search --- */

/*
 * chatview_search — Set an initial search pattern.
 *
 * Compiles the pattern and jumps to the first match from cursor.
 * Call before chatview_run() for /browse <pattern>.
 */
void chatview_search(chatview_t *cv, const char *pattern);

/*
 * chatview_search_forward — Find next match from index 'from'.
 * Returns message index or -1 if not found.
 */
int chatview_search_forward(const chatview_t *cv, int from);

/*
 * chatview_search_backward — Find previous match from index 'from'.
 * Returns message index or -1 if not found.
 */
int chatview_search_backward(const chatview_t *cv, int from);

/* --- Extension points --- */

/*
 * chatview_set_key_handler — Register a callback for custom keybindings.
 *
 * Called before the default handler. Return CHATVIEW_KEY_HANDLED to
 * consume the key, CHATVIEW_KEY_UNHANDLED to fall through to defaults,
 * or CHATVIEW_KEY_QUIT to exit the event loop.
 */
void chatview_set_key_handler(chatview_t *cv, chatview_key_handler_t handler,
                              void *userdata);

/*
 * chatview_set_poll — Register a periodic poll callback.
 *
 * Called on each read timeout (~100ms) during the event loop.
 * Use this to check for new messages and call chatview_update().
 */
void chatview_set_poll(chatview_t *cv, chatview_poll_fn fn, void *userdata);

/* --- Utility (for use by key handler callbacks) --- */

/*
 * chatview_set_status — Set the status bar message (printf-style).
 */
void chatview_set_status(chatview_t *cv, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * chatview_read_key — Read a single key from stdin.
 *
 * Returns a key code (ASCII char, or CHATVIEW_KEY_* enum value).
 * Returns CHATVIEW_KEY_NONE on timeout.
 */
int chatview_read_key(void);

/*
 * chatview_new_count — Number of messages arrived since init.
 */
static inline int chatview_new_count(const chatview_t *cv) {
    int diff = cv->state.message_count - cv->initial_count;
    return diff > 0 ? diff : 0;
}

/*
 * chatview_content_rows — Number of message rows visible.
 */
static inline int chatview_content_rows(const chatview_t *cv) {
    return cv->term_rows - 3; /* header + status bar + help bar */
}

#endif /* NBS_CHATVIEW_H */
