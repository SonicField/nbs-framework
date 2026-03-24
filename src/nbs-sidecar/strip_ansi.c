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
                /* CSI sequence: ESC [ ... final_byte (0x40-0x7E).
                 * Special case: cursor right (CSI <n> C) is used by
                 * Claude's terminal to render spaces. Replace with
                 * actual spaces instead of stripping. */
                rd++;
                /* Parse optional numeric parameter */
                int param = 0;
                int has_param = 0;
                const char *param_start = rd;
                while (*rd != '\0' && ((unsigned char)*rd < 0x40 || (unsigned char)*rd > 0x7E)) {
                    if (*rd >= '0' && *rd <= '9') {
                        param = param * 10 + (*rd - '0');
                        has_param = 1;
                    } else if (*rd == ';') {
                        /* Multiple params — not a simple cursor right */
                        has_param = 0;
                    }
                    rd++;
                }
                if (*rd == 'C' && (has_param || rd == param_start)) {
                    /* Cursor right: CSI C (1 space) or CSI <n> C */
                    int spaces = has_param ? param : 1;
                    if (spaces > 8) spaces = 8; /* cap to prevent abuse */
                    for (int i = 0; i < spaces; i++) *wr++ = ' ';
                }
                /* else: strip the sequence */
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
        } else if ((unsigned char)*rd >= 0xC2 && (unsigned char)*rd <= 0xF4) {
            /* UTF-8 multi-byte sequence. Copy the entire sequence as a
             * unit so that continuation bytes (0x80-0xBF) are not
             * mistakenly stripped by the C1 branch below.
             *
             * Leading byte ranges per RFC 3629:
             *   0xC2-0xDF: 2-byte sequence (1 continuation byte)
             *   0xE0-0xEF: 3-byte sequence (2 continuation bytes)
             *   0xF0-0xF4: 4-byte sequence (3 continuation bytes)
             *
             * 0xC0-0xC1 are overlong encodings and excluded.
             * If continuation bytes are missing or invalid, the leading
             * byte is still copied (garbage in, garbage out — but we
             * do not corrupt the stream). */
            int expected;
            if ((unsigned char)*rd <= 0xDF)      expected = 1;
            else if ((unsigned char)*rd <= 0xEF) expected = 2;
            else                                  expected = 3;

            *wr++ = *rd++;  /* copy leading byte */
            for (int i = 0; i < expected && *rd != '\0' &&
                 ((unsigned char)*rd & 0xC0) == 0x80; i++) {
                *wr++ = *rd++;  /* copy continuation byte */
            }
        } else if ((unsigned char)*rd >= 0x80 && (unsigned char)*rd <= 0x9F) {
            /* Strip standalone C1 control codes (0x80-0x9F).
             * These are the 8-bit equivalents of ESC-initiated sequences.
             * Applications inside terminal sessions may emit these.
             * This branch is only reached for bytes 0x80-0x9F that are NOT
             * preceded by a valid UTF-8 leading byte (those are handled
             * above). Standalone bytes in this range are C1 controls per
             * ISO 8859-1 and are stripped.
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
