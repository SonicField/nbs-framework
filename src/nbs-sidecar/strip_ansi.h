/*
 * strip_ansi.h — Strip ANSI/terminal escape sequences from text.
 *
 * Used by the @mention? query feature to clean tmux pane captures
 * before posting to chat.
 */

#ifndef STRIP_ANSI_H
#define STRIP_ANSI_H

#include <stddef.h>

/*
 * strip_ansi — Remove ANSI escape sequences in-place.
 *
 * Handles:
 *   - CSI sequences: ESC [ ... final_byte (0x40-0x7E)
 *   - OSC sequences: ESC ] ... ST (ESC \ or BEL)
 *   - Simple escapes: ESC followed by single char in 0x20-0x7E
 *
 * The string is modified in-place. The result is always <= the input length.
 * Returns the new length.
 */
size_t strip_ansi(char *text);

#endif /* STRIP_ANSI_H */
