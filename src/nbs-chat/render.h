/*
 * render.h — Shared ANSI colour rendering for chat messages
 *
 * Used by both nbs-chat-terminal (interactive display) and
 * nbs-chat export (file/stdout export). Single source of truth
 * for how chat messages look.
 *
 * Terminal attributes and colour codes come from nbs_term_attr.h.
 */

#ifndef NBS_CHAT_RENDER_H
#define NBS_CHAT_RENDER_H

#include "../nbs-common/nbs_term_attr.h"
#include <stdio.h>
#include <time.h>

/* String constants for direct use in printf format strings.
 * These are convenience aliases wrapping nbs_term_attr's bitmask API
 * for the common case where a single attribute is toggled inline. */
#define RENDER_BOLD      "\033[1m"
#define RENDER_DIM       "\033[2m"
#define RENDER_RESET     "\033[0m"
#define RENDER_ITALIC    "\033[3m"
#define RENDER_UNDERLINE "\033[4m"
#define RENDER_BLINK     "\033[5m"
#define RENDER_REVERSE   "\033[7m"
#define RENDER_STRIKE    "\033[9m"

/* Named 256-colour foreground strings for inline use */
#define RENDER_RED       "\033[38;5;196m"
#define RENDER_GREEN     "\033[38;5;41m"
#define RENDER_YELLOW    "\033[38;5;226m"
#define RENDER_BLUE      "\033[38;5;39m"
#define RENDER_CYAN      "\033[38;5;87m"

/*
 * render_init — Reset the handle-to-colour mapping.
 *
 * Call before rendering a new file/session so colours are assigned
 * consistently from the start of the conversation.
 */
void render_init(void);

/*
 * render_get_colour — Get the ANSI colour SGR parameter for a handle.
 *
 * Assigns colours on first use, returns the same colour for the
 * same handle on subsequent calls. Thread-unsafe (main thread only).
 *
 * Preconditions:
 *   - handle != NULL
 *
 * Returns: ANSI colour parameter string (e.g. "38;5;39")
 */
const char *render_get_colour(const char *handle);

/*
 * render_message — Render a chat message with ANSI colours.
 *
 * Format: "  [timestamp] handle: content\n"
 * Handle is bold + coloured. Timestamp is dim.
 *
 * Preconditions:
 *   - handle != NULL
 *   - content != NULL
 *   - out != NULL
 */
void render_message(const char *handle, const char *content,
                    time_t timestamp, FILE *out);

/*
 * render_message_own — Render own message with background highlight.
 *
 * Dark grey background strip with warm cream handle and light grey content.
 * Full-width background via \033[K (erase to end of line with current bg).
 *
 * Preconditions:
 *   - handle != NULL
 *   - content != NULL
 *   - out != NULL
 */
void render_message_own(const char *handle, const char *content,
                        time_t timestamp, FILE *out);

/*
 * render_message_bracket — Render a bracket-handle message with a given style.
 *
 * Styled bold handle, normal content. Used for [MEDIC-WARNING],
 * [SIDECAR-ERROR], and any future bracket handle types.
 *
 * Preconditions:
 *   - handle != NULL
 *   - content != NULL
 *   - style != NULL
 *   - out != NULL
 */
void render_message_bracket(const char *handle, const char *content,
                            time_t timestamp, const nbs_style_t *style,
                            FILE *out);

/*
 * render_set_highlight_handle — Set the handle for @mention highlighting.
 *
 * When set, @handle in message content is rendered with inverse video.
 * Pass NULL to disable highlighting.
 *
 * Preconditions:
 *   - handle is NULL or points to a valid C string
 */
void render_set_highlight_handle(const char *handle);

#endif /* NBS_CHAT_RENDER_H */
