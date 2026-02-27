/*
 * strip_ansi.c — Strip ANSI/terminal escape sequences and bare control
 *                characters from text.
 */

#include "strip_ansi.h"

#include <string.h>

#include "../nbs-common/nbs_assert.h"

size_t strip_ansi(char *text) {
    ASSERT_MSG(text != NULL,
               "strip_ansi: text must be non-NULL — caller passed NULL pointer. "
               "Check the caller's buffer allocation or argument passing.");

    size_t original_len = strlen(text);  /* capture for postcondition */
    char *rd = text;
    char *wr = text;

    while (*rd != '\0') {
        ASSERT_MSG(wr <= rd,
                   "strip_ansi: write pointer %p overtook read pointer %p — "
                   "in-place stripping invariant violated, output would corrupt unread data",
                   (void *)wr, (void *)rd);

        if (*rd == '\x1b') {
            rd++; /* Skip ESC */
            if (*rd == '[') {
                /* CSI sequence: ESC [ ... final_byte (0x40-0x7E) */
                rd++;
                while (*rd != '\0' && ((unsigned char)*rd < 0x40 || (unsigned char)*rd > 0x7E)) {
                    rd++;
                }
                if (*rd != '\0') rd++; /* Skip final byte */
            } else if (*rd == ']') {
                /* OSC sequence: ESC ] ... ST (ESC \ or BEL) */
                rd++;
                while (*rd != '\0') {
                    if (*rd == '\x07') { /* BEL */
                        rd++;
                        break;
                    }
                    if (*rd == '\x1b' && *(rd + 1) == '\\') { /* ESC \ */
                        rd += 2;
                        break;
                    }
                    rd++;
                }
            } else if ((unsigned char)*rd >= 0x20 && (unsigned char)*rd <= 0x7E) {
                /* Simple escape: ESC + single char */
                rd++;
            }
            /* else: bare ESC at end of string — skip it */
        } else if (((unsigned char)*rd < 0x20 &&
                    *rd != '\n' && *rd != '\t') ||
                   (unsigned char)*rd == 0x7F) {
            /* Strip bare control characters (CR, BEL, DEL, etc.)
             * but preserve newlines and tabs.
             * DEL (0x7F) is a control character that appears in terminal
             * captures and must not pass through to chat output. */
            rd++;
        } else if ((unsigned char)*rd >= 0x80 && (unsigned char)*rd <= 0x9F) {
            /* Strip C1 control codes (0x80-0x9F).
             * These are the 8-bit equivalents of ESC-initiated sequences.
             * Applications inside tmux may emit these. They are not valid
             * in UTF-8 text (0x80-0x9F are continuation bytes in UTF-8,
             * but standalone they are C1 controls per ISO 8859-1).
             * 0x9B (CSI) and 0x9D (OSC) could introduce sequences, but
             * stripping the introducer byte is sufficient since the
             * remaining bytes are either printable or caught by other
             * branches. */
            rd++;
        } else {
            *wr++ = *rd++;
        }
    }

    *wr = '\0';
    size_t new_len = (size_t)(wr - text);
    ASSERT_MSG(new_len <= original_len,
               "strip_ansi: output length %zu exceeds input length %zu — "
               "stripping must never grow the string",
               new_len, original_len);
    return new_len;
}
