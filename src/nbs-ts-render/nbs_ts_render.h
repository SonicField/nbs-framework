/*
 * nbs_ts_render.h — Virtual terminal emulator: public API.
 *
 * Maintains an internal screen buffer (fixed cell grid) and processes
 * raw PTY output byte-by-byte through a VT100/xterm state machine.
 * Strips decoration escapes (color, bold, italic, underline) but
 * respects positional commands (cursor movement, scrolling, erase).
 *
 * Output: the final screen state as plain UTF-8 text.
 */

#ifndef NBS_TS_RENDER_H
#define NBS_TS_RENDER_H

#include <stddef.h>

/* Default PTY size matching nbs-ts-helper (helper.c:162) */
#define NBS_TS_RENDER_DEFAULT_COLS 80
#define NBS_TS_RENDER_DEFAULT_ROWS 24

/* Maximum UTF-8 bytes per cell */
#define NBS_TS_RENDER_CELL_BYTES 4

/* Maximum CSI parameters */
#define NBS_TS_RENDER_MAX_PARAMS 16

/* State machine states (VT100/xterm parser) */
typedef enum {
    STATE_GROUND,         /* Normal text processing */
    STATE_ESC,            /* After ESC (0x1B) */
    STATE_ESC_INTER,      /* ESC + intermediate byte(s) (0x20-0x2F) */
    STATE_CSI_PARAM,      /* After ESC [ — collecting parameters */
    STATE_CSI_INTER,      /* CSI + intermediate byte(s) */
    STATE_OSC,            /* After ESC ] — operating system command */
    STATE_OSC_ESC,        /* OSC string, saw ESC (possible ST = ESC \) */
    STATE_DCS,            /* After ESC P — device control string */
    STATE_DCS_ESC,        /* DCS string, saw ESC */
} ts_render_state_t;

/* A single screen cell */
typedef struct {
    char ch[NBS_TS_RENDER_CELL_BYTES]; /* UTF-8 codepoint (NUL-padded) */
    int  len;                          /* byte length of ch (0 = empty) */
} ts_render_cell_t;

/* The terminal emulator context */
typedef struct {
    int rows;
    int cols;

    /* Screen buffer: rows * cols cells */
    ts_render_cell_t *cells;

    /* Cursor position (0-based) */
    int cursor_row;
    int cursor_col;

    /* State machine */
    ts_render_state_t state;

    /* CSI parameter accumulation */
    int  params[NBS_TS_RENDER_MAX_PARAMS];
    int  param_count;
    int  param_val;       /* current parameter being parsed */
    int  param_has_val;   /* whether current param has digits */
    char intermediate;    /* intermediate byte (0x20-0x2F), 0 if none */

    /* Scroll region (top and bottom, 0-based inclusive) */
    int scroll_top;
    int scroll_bottom;

    /* Auto-wrap: if cursor is past last column, next printable wraps */
    int pending_wrap;

    /* Saved cursor position (for ESC 7 / ESC 8) */
    int saved_cursor_row;
    int saved_cursor_col;

    /* UTF-8 multi-byte accumulation */
    char   utf8_buf[NBS_TS_RENDER_CELL_BYTES];
    int    utf8_len;      /* bytes accumulated so far */
    int    utf8_expect;   /* total bytes expected */

    /* Tab stops (bitfield: 1 bit per column) */
    unsigned char *tab_stops;
} ts_render_t;

/*
 * Create a new terminal emulator with the given dimensions.
 * Returns NULL on allocation failure.
 */
ts_render_t *ts_render_create(int rows, int cols);

/*
 * Destroy a terminal emulator and free all memory.
 */
void ts_render_destroy(ts_render_t *t);

/*
 * Feed raw bytes into the terminal emulator.
 * Processes each byte through the state machine.
 */
void ts_render_feed(ts_render_t *t, const char *data, size_t len);

/*
 * Render the current screen buffer as plain text.
 * Trailing spaces on each line are trimmed. Lines separated by '\n'.
 * Returns a malloc'd string (caller frees). Returns NULL on failure.
 */
char *ts_render_snapshot(const ts_render_t *t);

/*
 * Reset the terminal to initial state (clear screen, home cursor).
 */
void ts_render_reset(ts_render_t *t);

#endif /* NBS_TS_RENDER_H */
