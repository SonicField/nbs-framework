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
 *   - Bare control characters: C0 (0x00-0x1F except \n, \t) and DEL (0x7F)
 *   - C1 control codes: standalone 0x80-0x9F (8-bit control characters)
 *
 * UTF-8 aware: multi-byte sequences (leading byte 0xC2-0xF4 followed
 * by continuation bytes 0x80-0xBF) are copied as a unit. Continuation
 * bytes in the C1 range are NOT stripped when part of a valid sequence.
 *
 * The string is modified in-place. Returns the new length.
 *
 * Postcondition enforced by assertion: output length <= input length.
 * Invariant enforced by assertion: write pointer never overtakes read pointer.
 */
size_t strip_ansi(char *text);

#endif /* STRIP_ANSI_H */
