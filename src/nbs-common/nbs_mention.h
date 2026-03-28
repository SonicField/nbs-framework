/*
 * nbs_mention.h — Shared @mention matching logic.
 *
 * Used by bus_bridge.c (mention extraction for bus events) and
 * render.c (@mention highlighting in chat display). Single source
 * of truth for what constitutes a valid handle character and what
 * distinguishes a @mention from an email address.
 *
 * All functions are static inline — no .c file needed.
 */

#ifndef NBS_MENTION_H
#define NBS_MENTION_H

#include <ctype.h>

/*
 * nbs_is_handle_char — Returns true if c is valid in a @handle.
 *
 * Handles can contain: a-z, A-Z, 0-9, underscore, hyphen.
 */
static inline int nbs_is_handle_char(int c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

/*
 * nbs_is_email_prefix_char — Returns true if c can precede @ in an email.
 *
 * Email local parts can contain: a-z, A-Z, 0-9, dot, underscore, hyphen, plus.
 * If the character before @ is one of these, it's likely an email, not a mention.
 */
static inline int nbs_is_email_prefix_char(int c) {
    return isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '+';
}

#endif /* NBS_MENTION_H */
