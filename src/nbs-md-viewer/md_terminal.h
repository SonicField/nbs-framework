/*
 * md_terminal.h — Terminal management for nbs-md-viewer.
 *
 * Raw mode, SIGWINCH handling, and keyboard input.
 */

#ifndef MD_TERMINAL_H
#define MD_TERMINAL_H

typedef enum {
    MD_KEY_UP,
    MD_KEY_DOWN,
    MD_KEY_LEFT,
    MD_KEY_RIGHT,
    MD_KEY_PAGE_UP,
    MD_KEY_PAGE_DOWN,
    MD_KEY_HOME,
    MD_KEY_END,
    MD_KEY_QUIT,
    MD_KEY_UNKNOWN
} md_key_t;

/* Enter raw mode: alternate screen, hide cursor, raw termios.
 * Installs SIGWINCH handler and atexit cleanup.
 * Returns 0 on success, -1 on failure. */
int md_terminal_enter_raw(void);

/* Leave raw mode: restore termios, show cursor, primary screen.
 * Safe to call multiple times. */
void md_terminal_leave_raw(void);

/* Read a single keypress (blocking). Returns the key type. */
md_key_t md_terminal_read_key(void);

/* Get terminal dimensions. Returns 0 on success, -1 on failure. */
int md_terminal_get_size(int *rows, int *cols);

/* Check if a resize has occurred since last check (and clear the flag). */
int md_terminal_resize_pending(void);

#endif /* MD_TERMINAL_H */
