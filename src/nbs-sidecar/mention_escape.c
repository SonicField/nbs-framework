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
 *
 * Enforced by test_sidecar_escape_mentions_unit test 15
 * (256-byte sync check against bus_extract_mentions).
 */
static int is_mention_handle_char(int c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

/*
 * AT_REPLACEMENT — The character that replaces '@' in sanitise_at_signs.
 *
 * Must be:
 *   - A single byte (in-place replacement, no length change)
 *   - Not '@' (defeats the purpose)
 *   - Not a handle character (would not affect mention extraction)
 *   - Valid printable ASCII (safe for any downstream consumer including
 *     UTF-8 validators, chat renderers, and log parsers)
 *
 * '#' satisfies all constraints: printable, not a handle char, valid
 * ASCII/UTF-8, and visually distinct from '@'.
 */
#define AT_REPLACEMENT '#'

void sanitise_at_signs(char *content) {
    ASSERT_MSG(content != NULL, "sanitise_at_signs: content is NULL");

    for (char *p = content; *p != '\0'; p++) {
        if (*p == '@') *p = AT_REPLACEMENT;
    }

    /* Postcondition: no '@' characters remain */
    for (const char *q = content; *q != '\0'; q++) {
        ASSERT_MSG(*q != '@',
                   "sanitise_at_signs: postcondition violated, '@' remains at offset %td — "
                   "the replacement loop missed a character",
                   q - content);
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

    /*
     * malloc failure is treated as a fatal error (abort via ASSERT_MSG).
     * This is deliberate project policy: OOM in the sidecar is unrecoverable.
     * The sidecar is a short-lived helper process; there is no meaningful
     * degradation path when the OS cannot provide a few hundred bytes.
     */
    char *out = malloc(len + extra + 1);
    ASSERT_MSG(out != NULL,
               "escape_mentions: malloc failed for %zu bytes — "
               "OOM in sidecar is fatal by design (no degradation path)",
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
