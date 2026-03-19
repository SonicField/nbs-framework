/*
 * render.h — Shared ANSI colour rendering for chat messages
 *
 * Used by both nbs-chat-terminal (interactive display) and
 * nbs-chat export (file/stdout export). Single source of truth
 * for how chat messages look.
 */

#ifndef NBS_CHAT_RENDER_H
#define NBS_CHAT_RENDER_H

#include <stdio.h>
#include <time.h>

/* ANSI escape constants */
#define RENDER_BOLD  "\033[1m"
#define RENDER_DIM   "\033[2m"
#define RENDER_RESET "\033[0m"

/*
 * render_init — Reset the handle-to-colour mapping.
 *
 * Call before rendering a new file/session so colours are assigned
 * consistently from the start of the conversation.
 */
void render_init(void);

/*
 * render_get_colour — Get the ANSI colour code for a handle.
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
 * render_message_own — Render own message (dimmer variant).
 *
 * Same layout but handle and content are dimmed, for distinguishing
 * the user's own messages in interactive mode.
 *
 * Preconditions:
 *   - handle != NULL
 *   - content != NULL
 *   - out != NULL
 */
void render_message_own(const char *handle, const char *content,
                        time_t timestamp, FILE *out);

#endif /* NBS_CHAT_RENDER_H */
