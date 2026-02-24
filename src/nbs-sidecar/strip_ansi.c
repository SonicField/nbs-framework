/*
 * strip_ansi.c — Strip ANSI/terminal escape sequences and bare control
 *                characters from text.
 */

#include "strip_ansi.h"

#include <string.h>

#include "../nbs-common/nbs_assert.h"

size_t strip_ansi(char *text) {
    ASSERT_MSG(text != NULL, "strip_ansi: text is NULL");

    char *rd = text;
    char *wr = text;

    while (*rd != '\0') {
        if (*rd == '\x1b') {
            rd++; /* Skip ESC */
            if (*rd == '[') {
                /* CSI sequence: ESC [ ... final_byte (0x40-0x7E) */
                rd++;
                while (*rd != '\0' && (*rd < 0x40 || *rd > 0x7E)) {
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
            } else if (*rd >= 0x20 && *rd <= 0x7E) {
                /* Simple escape: ESC + single char */
                rd++;
            }
            /* else: bare ESC at end of string — skip it */
        } else if ((unsigned char)*rd < 0x20 &&
                   *rd != '\n' && *rd != '\t') {
            /* Strip bare control characters (CR, BEL, etc.)
             * but preserve newlines and tabs */
            rd++;
        } else {
            *wr++ = *rd++;
        }
    }

    *wr = '\0';
    return (size_t)(wr - text);
}
