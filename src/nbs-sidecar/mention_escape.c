/*
 * mention_escape.c — Escape @mentions to prevent feedback loops.
 *
 * Two independent escaping functions for defence in depth:
 *   sanitise_at_signs: brute-force replacement of all @ (layer 1)
 *   escape_mentions: targeted insertion of \ after @ before handles (layer 2)
 */

#include "mention_escape.h"
#include "../nbs-common/nbs_assert.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * is_mention_handle_char — Returns true if c is valid in a @handle.
 *
 * MUST match is_handle_char in bus_bridge.c exactly:
 *   isalnum(c) || c == '_' || c == '-'
 *
 * If these diverge, escape_mentions may miss patterns that
 * bus_extract_mentions would accept. sanitise_at_signs (layer 1)
 * provides brute-force coverage regardless.
 */
static int is_mention_handle_char(int c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

void sanitise_at_signs(char *content) {
    ASSERT_MSG(content != NULL, "sanitise_at_signs: content is NULL");

    for (char *p = content; *p != '\0'; p++) {
        if (*p == '@') *p = '\xc0';
    }
}

char *escape_mentions(const char *input) {
    ASSERT_MSG(input != NULL, "escape_mentions: input is NULL");

    size_t len = strlen(input);

    /* Count @ signs followed by handle chars to size the output buffer.
     * Each such occurrence needs one extra byte for the inserted '\'. */
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
        if (input[i] == '@' &&
            i + 1 < len &&
            is_mention_handle_char((unsigned char)input[i + 1])) {
            extra++;
        }
    }

    char *out = malloc(len + extra + 1);
    ASSERT_MSG(out != NULL,
               "escape_mentions: malloc failed for %zu bytes",
               len + extra + 1);

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        out[j++] = input[i];
        if (input[i] == '@' &&
            i + 1 < len &&
            is_mention_handle_char((unsigned char)input[i + 1])) {
            out[j++] = '\\';
        }
    }
    out[j] = '\0';

    /* Postcondition: output length matches the pre-computed size */
    ASSERT_MSG(j == len + extra,
               "escape_mentions: output length %zu != expected %zu",
               j, len + extra);

    return out;
}
