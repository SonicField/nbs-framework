/*
 * mention_escape.h — Escape @mentions in pane captures to prevent feedback loops.
 *
 * When @handle? triggers a pane capture, the captured text may contain
 * @handle references. If posted to chat unescaped, bus_extract_mentions
 * would extract them as new mentions, causing an infinite loop.
 *
 * Two independent escaping layers:
 *   1. sanitise_at_signs: replaces all @ with \xc0 in-place (brute force)
 *   2. escape_mentions: inserts \ after @ before handle chars (targeted)
 *
 * After sanitise_at_signs, escape_mentions is a no-op — its value is as
 * an independently testable fallback if sanitise_at_signs is ever relaxed.
 */

#ifndef NBS_MENTION_ESCAPE_H
#define NBS_MENTION_ESCAPE_H

/*
 * sanitise_at_signs — Replace all '@' with '\xc0' in-place.
 *
 * Preconditions:
 *   - content != NULL
 *   - content is a NUL-terminated, mutable string
 *
 * Postconditions:
 *   - No '@' characters remain in content
 *   - String length is unchanged
 */
void sanitise_at_signs(char *content);

/*
 * escape_mentions — Insert '\' after '@' when followed by a handle character.
 *
 * Handle characters: [a-zA-Z0-9_-] (matches is_handle_char in bus_bridge.c).
 *
 * Preconditions:
 *   - input != NULL
 *   - input is NUL-terminated
 *
 * Postconditions:
 *   - Returns a malloc'd NUL-terminated string (caller must free)
 *   - Every '@' followed by a handle char has '\' inserted between them
 *   - '@' followed by non-handle char or at end of string is unchanged
 *   - Output passed to bus_extract_mentions yields 0 mentions
 */
char *escape_mentions(const char *input);

#endif /* NBS_MENTION_ESCAPE_H */
