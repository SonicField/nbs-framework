/*
 * mention_escape.h — Escape @mentions in pane captures to prevent feedback loops.
 *
 * When @handle? triggers a pane capture, the captured text may contain
 * @handle references. If posted to chat unescaped, bus_extract_mentions
 * would extract them as new mentions, causing an infinite loop.
 *
 * Two independent escaping layers:
 *   1. sanitise_at_signs: replaces all @ with '#' in-place (brute force)
 *   2. escape_mentions: inserts \ after @ before handle chars (targeted)
 *
 * After sanitise_at_signs, escape_mentions is a no-op — its value is as
 * an independently testable fallback if sanitise_at_signs is ever relaxed.
 */

#ifndef NBS_MENTION_ESCAPE_H
#define NBS_MENTION_ESCAPE_H

/*
 * sanitise_at_signs — Replace all '@' with '#' in-place.
 *
 * The replacement character '#' is printable ASCII, not a handle
 * character, and valid in all downstream encoding contexts (UTF-8, etc.).
 *
 * Preconditions:
 *   - content != NULL
 *   - content is a NUL-terminated, mutable string
 *
 * Postconditions (enforced by assertion):
 *   - No '@' characters remain in content
 *   - String length is unchanged
 */
void sanitise_at_signs(char *content);

/*
 * escape_mentions — Insert '\' after '@' when followed by a handle character.
 *
 * Handle characters: [a-zA-Z0-9_-] (matches is_handle_char in bus_bridge.c).
 * Sync enforced by test_sidecar_escape_mentions_unit test 15 (256-byte check).
 *
 * Preconditions:
 *   - input != NULL
 *   - input is NUL-terminated
 *
 * Postconditions (enforced by assertion):
 *   - Returns a malloc'd NUL-terminated string (caller must free)
 *   - Every '@' followed by a handle char has '\' inserted between them
 *   - '@' followed by non-handle char or at end of string is unchanged
 *   - Output length matches pre-computed expected size
 */
char *escape_mentions(const char *input);

#endif /* NBS_MENTION_ESCAPE_H */
