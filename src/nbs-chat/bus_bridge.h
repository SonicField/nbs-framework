/*
 * bus_bridge.h — Chat-to-bus event bridge
 *
 * When .nbs/events/ exists relative to the chat file's project root,
 * nbs-chat send publishes bus events via nbs-bus publish.
 *
 * Design decisions:
 *   - Shells out to nbs-bus binary (single source of truth for event format)
 *   - Bus failure never causes chat send failure (graceful degradation)
 *   - @mentions detected by scanning message for @word patterns
 *   - Email-like patterns (char@word) are excluded from @mention detection
 */

#ifndef NBS_BUS_BRIDGE_H
#define NBS_BUS_BRIDGE_H

#include "chat_file.h"

/*
 * Maximum mentions extractable from a single message.
 * Exceeding this silently drops additional mentions — not an error.
 */
#define MAX_MENTIONS 16

/*
 * Maximum length of a handle extracted from an @mention.
 * Matches MAX_HANDLE_LEN from chat_file.h.
 */
#define MAX_MENTION_HANDLE_LEN MAX_HANDLE_LEN

/*
 * bus_bridge_after_send — Publish bus events after a chat message is sent.
 *
 * Preconditions:
 *   - chat_path != NULL (path to the chat file that was written to)
 *   - handle != NULL (sender handle)
 *   - message != NULL (message content that was sent)
 *
 * Postconditions:
 *   - If .nbs/events/ exists: attempts to publish chat-message event.
 *     If message contains @mentions: also publishes chat-mention events.
 *     If message contains @handle! (interrupt): publishes chat-interrupt
 *     events at critical priority instead of chat-mention.
 *   - If .nbs/events/ does not exist: returns silently with no side effects.
 *   - Bus publish failures are logged to stderr but return 0 (success).
 *   - Returns 0 always — bus bridge never fails the caller.
 *
 * Note: "never fails" is a design invariant, not an implementation detail.
 * If the bus is down, broken, misconfigured, or missing, chat must still work.
 */
int bus_bridge_after_send(const char *chat_path, const char *handle,
                          const char *message);

/*
 * bus_find_events_dir — Locate .nbs/events/ from a chat file path.
 *
 * Preconditions:
 *   - chat_path != NULL
 *   - out_buf != NULL
 *   - out_buf_size > 0
 *
 * Postconditions:
 *   - If found: writes absolute path to out_buf, returns 0
 *   - If not found: returns -1, out_buf contents undefined
 *
 * Strategy: chat files are in .nbs/chat/. Walk up from the chat file's
 * directory to find .nbs/, then check for events/ within it.
 */
int bus_find_events_dir(const char *chat_path, char *out_buf, size_t out_buf_size);

/*
 * bus_bridge_human_input — Publish a high-priority human-input bus event.
 *
 * Called by nbs-chat-terminal after a human sends a message. This is in
 * addition to the standard chat-message event published by
 * bus_bridge_after_send(). The human-input event signals to AI agents
 * that a human has spoken and their attention is needed.
 *
 * Preconditions:
 *   - chat_path != NULL (path to the chat file)
 *   - handle != NULL (sender handle)
 *   - message != NULL (message content)
 *
 * Postconditions:
 *   - If .nbs/events/ exists: publishes human-input event (high priority).
 *   - If .nbs/events/ does not exist: returns silently.
 *   - Returns 0 always — bus bridge never fails the caller.
 */
int bus_bridge_human_input(const char *chat_path, const char *handle,
                            const char *message);

/*
 * bus_extract_mentions — Extract @handles from a message.
 *
 * Preconditions:
 *   - message != NULL
 *   - out_handles != NULL, array of at least max_handles char arrays
 *   - Each char array in out_handles has at least MAX_MENTION_HANDLE_LEN bytes
 *   - max_handles > 0
 *   - out_interrupt_flags may be NULL (interrupt detection disabled)
 *     or an int array of at least max_handles elements
 *
 * Postconditions:
 *   - Returns the number of unique handles found (0 to max_handles)
 *   - Each out_handles[i] for i < return value contains a NUL-terminated handle
 *   - If out_interrupt_flags != NULL: out_interrupt_flags[i] is 1 if the
 *     mention was followed by '!' (interrupt pattern), 0 otherwise
 *   - Email-like patterns (preceded by [a-zA-Z0-9._]) are excluded
 *   - Duplicate handles are excluded (only first occurrence counts)
 *
 * An @mention is: @ preceded by whitespace or start-of-string,
 * followed by [a-zA-Z0-9_-]+.
 *
 * An @mention! (interrupt) is: @handle immediately followed by '!'.
 * The '!' is not part of the handle — it signals an interrupt request.
 */
int bus_extract_mentions(const char *message,
                         char out_handles[][MAX_MENTION_HANDLE_LEN],
                         int max_handles,
                         int *out_interrupt_flags);

#endif /* NBS_BUS_BRIDGE_H */
