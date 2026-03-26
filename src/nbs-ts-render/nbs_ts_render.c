/*
 * nbs_ts_render.c — Virtual terminal emulator core.
 *
 * State machine processes raw PTY output byte-by-byte.
 * Strips decoration (SGR, color, bold, italic, underline).
 * Maintains cursor position, scrolling, erase, and screen buffer.
 */

#include "nbs_ts_render.h"
#include "nbs_ts_wcwidth.h"
#include "nbs_ts_bidi.h"
#include "../nbs-common/nbs_assert.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static ts_render_cell_t *cell_at(ts_render_t *t, int row, int col) {
    ASSERT_MSG(row >= 0 && row < t->rows,
               "cell_at: row %d out of range [0, %d)", row, t->rows);
    ASSERT_MSG(col >= 0 && col < t->cols,
               "cell_at: col %d out of range [0, %d)", col, t->cols);
    return &t->cells[row * t->cols + col];
}

static const ts_render_cell_t *cell_at_const(const ts_render_t *t, int row, int col) {
    ASSERT_MSG(row >= 0 && row < t->rows,
               "cell_at_const: row %d out of range [0, %d)", row, t->rows);
    ASSERT_MSG(col >= 0 && col < t->cols,
               "cell_at_const: col %d out of range [0, %d)", col, t->cols);
    return &t->cells[row * t->cols + col];
}

static void clear_cell(ts_render_cell_t *c) {
    memset(c->ch, 0, NBS_TS_RENDER_CELL_BYTES);
    c->len = 0;
}

static void clear_row(ts_render_t *t, int row) {
    for (int col = 0; col < t->cols; col++) {
        clear_cell(cell_at(t, row, col));
    }
}

static int clamp(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static int csi_param(ts_render_t *t, int idx, int default_val) {
    if (idx < t->param_count && t->params[idx] > 0)
        return t->params[idx];
    return default_val;
}

static void init_tab_stops(ts_render_t *t) {
    int bytes = (t->cols + 7) / 8;
    memset(t->tab_stops, 0, (size_t)bytes);
    for (int col = 0; col < t->cols; col += 8) {
        t->tab_stops[col / 8] |= (unsigned char)(1 << (col % 8));
    }
}

static int is_tab_stop(const ts_render_t *t, int col) {
    if (col < 0 || col >= t->cols) return 0;
    return (t->tab_stops[col / 8] >> (col % 8)) & 1;
}

/* ── Scrolling ────────────────────────────────────────────────────── */

static void scroll_up(ts_render_t *t, int n) {
    ASSERT_MSG(n > 0, "scroll_up: n must be positive, got %d", n);
    int top = t->scroll_top;
    int bot = t->scroll_bottom;
    if (n > bot - top + 1) n = bot - top + 1;

    /* Move rows up */
    for (int row = top; row <= bot - n; row++) {
        memcpy(&t->cells[row * t->cols],
               &t->cells[(row + n) * t->cols],
               (size_t)t->cols * sizeof(ts_render_cell_t));
    }
    /* Clear vacated rows at bottom */
    for (int row = bot - n + 1; row <= bot; row++) {
        clear_row(t, row);
    }
}

static void scroll_down(ts_render_t *t, int n) {
    ASSERT_MSG(n > 0, "scroll_down: n must be positive, got %d", n);
    int top = t->scroll_top;
    int bot = t->scroll_bottom;
    if (n > bot - top + 1) n = bot - top + 1;

    /* Move rows down */
    for (int row = bot; row >= top + n; row--) {
        memcpy(&t->cells[row * t->cols],
               &t->cells[(row - n) * t->cols],
               (size_t)t->cols * sizeof(ts_render_cell_t));
    }
    /* Clear vacated rows at top */
    for (int row = top; row < top + n; row++) {
        clear_row(t, row);
    }
}

/* ── Line feed / reverse line feed ────────────────────────────────── */

static void do_linefeed(ts_render_t *t) {
    if (t->cursor_row == t->scroll_bottom) {
        scroll_up(t, 1);
    } else if (t->cursor_row < t->rows - 1) {
        t->cursor_row++;
    }
}

static void do_reverse_linefeed(ts_render_t *t) {
    if (t->cursor_row == t->scroll_top) {
        scroll_down(t, 1);
    } else if (t->cursor_row > 0) {
        t->cursor_row--;
    }
}

/* ── Wide/combining character helpers ─────────────────────────────── */

/* Continuation cell marker: len = -1 means second half of a wide char */
#define CELL_IS_CONTINUATION(c) ((c)->len == -1)

static void set_continuation(ts_render_cell_t *c) {
    memset(c->ch, 0, NBS_TS_RENDER_CELL_BYTES);
    c->len = -1;
}

/* Clear a cell and its paired wide-char cell if applicable */
static void clear_wide_cell(ts_render_t *t, int row, int col) {
    ts_render_cell_t *c = cell_at(t, row, col);
    if (CELL_IS_CONTINUATION(c)) {
        /* This is the right half — also clear the left (primary) */
        if (col > 0) clear_cell(cell_at(t, row, col - 1));
        clear_cell(c);
    } else if (c->len > 0 && col + 1 < t->cols) {
        /* Check if next cell is continuation (this is a wide char primary) */
        ts_render_cell_t *next = cell_at(t, row, col + 1);
        if (CELL_IS_CONTINUATION(next)) clear_cell(next);
        clear_cell(c);
    } else {
        clear_cell(c);
    }
}

/* Decode UTF-8 bytes to a Unicode codepoint */
static uint32_t utf8_to_codepoint(const char *ch, int len) {
    const unsigned char *u = (const unsigned char *)ch;
    if (len == 1) return u[0];
    if (len == 2) return ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    if (len == 3) return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    if (len == 4) return ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) | ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    return 0xFFFD; /* replacement character */
}

/* ── Put a printable character at cursor ──────────────────────────── */

static void put_char(ts_render_t *t, const char *ch, int len) {
    ASSERT_MSG(len > 0 && len <= NBS_TS_RENDER_CELL_BYTES,
               "put_char: invalid char length %d", len);

    uint32_t cp = utf8_to_codepoint(ch, len);
    int width = nbs_ts_wcwidth(cp);

    /* Width 0: combining mark — append to previous cell */
    if (width == 0) {
        int prev_col = t->cursor_col;
        if (t->pending_wrap) prev_col = t->cols - 1;
        else if (prev_col > 0) prev_col--;
        else return; /* no previous cell to attach to */

        /* Skip continuation cells to find the primary */
        ts_render_cell_t *prev = cell_at(t, t->cursor_row, prev_col);
        if (CELL_IS_CONTINUATION(prev) && prev_col > 0) {
            prev_col--;
            prev = cell_at(t, t->cursor_row, prev_col);
        }

        /* Append if there's room in the cell */
        if (prev->len > 0 && prev->len + len <= NBS_TS_RENDER_CELL_BYTES) {
            memcpy(prev->ch + prev->len, ch, (size_t)len);
            prev->len += len;
        }
        /* else: drop the combining mark (cell full) */
        return;
    }

    /* Handle pending wrap (auto-wrap mode) */
    if (t->pending_wrap) {
        t->cursor_col = 0;
        do_linefeed(t);
        t->pending_wrap = 0;
    }

    /* Width 2: wide character — needs special handling */
    if (width == 2) {
        /* If at last column, wide char doesn't fit — wrap first */
        if (t->cursor_col >= t->cols - 1) {
            /* Leave last column empty, wrap to next line */
            clear_cell(cell_at(t, t->cursor_row, t->cursor_col));
            t->cursor_col = 0;
            do_linefeed(t);
        }

        /* Clear any existing wide char at target cells */
        clear_wide_cell(t, t->cursor_row, t->cursor_col);
        clear_wide_cell(t, t->cursor_row, t->cursor_col + 1);

        /* Place the character in primary cell */
        ts_render_cell_t *c = cell_at(t, t->cursor_row, t->cursor_col);
        memcpy(c->ch, ch, (size_t)len);
        if (len < NBS_TS_RENDER_CELL_BYTES) {
            memset(c->ch + len, 0, (size_t)(NBS_TS_RENDER_CELL_BYTES - len));
        }
        c->len = len;

        /* Mark next cell as continuation */
        set_continuation(cell_at(t, t->cursor_row, t->cursor_col + 1));

        /* Advance cursor by 2 */
        if (t->cursor_col + 2 >= t->cols) {
            t->cursor_col = t->cols - 1;
            t->pending_wrap = 1;
        } else {
            t->cursor_col += 2;
        }
        return;
    }

    /* Width 1: normal character */
    /* Clear any existing wide char at target cell */
    clear_wide_cell(t, t->cursor_row, t->cursor_col);

    ts_render_cell_t *c = cell_at(t, t->cursor_row, t->cursor_col);
    memcpy(c->ch, ch, (size_t)len);
    if (len < NBS_TS_RENDER_CELL_BYTES) {
        memset(c->ch + len, 0, (size_t)(NBS_TS_RENDER_CELL_BYTES - len));
    }
    c->len = len;

    /* Advance cursor */
    if (t->cursor_col >= t->cols - 1) {
        t->pending_wrap = 1;
    } else {
        t->cursor_col++;
    }
}

/* ── CSI dispatch ─────────────────────────────────────────────────── */

static void csi_finalize(ts_render_t *t) {
    /* Push final accumulated parameter */
    if (t->param_has_val && t->param_count < NBS_TS_RENDER_MAX_PARAMS) {
        t->params[t->param_count++] = t->param_val;
    }
}

static void dispatch_csi(ts_render_t *t, char final_byte) {
    csi_finalize(t);
    int n;

    /* If there's a private-mode prefix (intermediate '?', '>', '!'),
     * skip — these are decoration/mode sequences we don't handle. */
    if (t->intermediate == '?' || t->intermediate == '>' || t->intermediate == '!') {
        return;
    }

    switch (final_byte) {
    /* ── Cursor movement ─────────────────────────────────────── */
    case 'A': /* CUU — Cursor Up */
        n = csi_param(t, 0, 1);
        t->cursor_row = clamp(t->cursor_row - n, t->scroll_top, t->scroll_bottom);
        t->pending_wrap = 0;
        break;

    case 'B': /* CUD — Cursor Down */
        n = csi_param(t, 0, 1);
        t->cursor_row = clamp(t->cursor_row + n, t->scroll_top, t->scroll_bottom);
        t->pending_wrap = 0;
        break;

    case 'C': /* CUF — Cursor Forward (Right) */
        n = csi_param(t, 0, 1);
        t->cursor_col = clamp(t->cursor_col + n, 0, t->cols - 1);
        t->pending_wrap = 0;
        break;

    case 'D': /* CUB — Cursor Backward (Left) */
        n = csi_param(t, 0, 1);
        t->cursor_col = clamp(t->cursor_col - n, 0, t->cols - 1);
        t->pending_wrap = 0;
        break;

    case 'E': /* CNL — Cursor Next Line */
        n = csi_param(t, 0, 1);
        t->cursor_row = clamp(t->cursor_row + n, t->scroll_top, t->scroll_bottom);
        t->cursor_col = 0;
        t->pending_wrap = 0;
        break;

    case 'F': /* CPL — Cursor Previous Line */
        n = csi_param(t, 0, 1);
        t->cursor_row = clamp(t->cursor_row - n, t->scroll_top, t->scroll_bottom);
        t->cursor_col = 0;
        t->pending_wrap = 0;
        break;

    case 'G': /* CHA — Cursor Horizontal Absolute */
        n = csi_param(t, 0, 1);
        t->cursor_col = clamp(n - 1, 0, t->cols - 1);
        t->pending_wrap = 0;
        break;

    case 'H': /* CUP — Cursor Position */
    case 'f': /* HVP — same as CUP */
    {
        int row = csi_param(t, 0, 1);
        int col = csi_param(t, 1, 1);
        t->cursor_row = clamp(row - 1, 0, t->rows - 1);
        t->cursor_col = clamp(col - 1, 0, t->cols - 1);
        t->pending_wrap = 0;
        break;
    }

    case 'd': /* VPA — Vertical Position Absolute */
        n = csi_param(t, 0, 1);
        t->cursor_row = clamp(n - 1, 0, t->rows - 1);
        t->pending_wrap = 0;
        break;

    /* ── Erase ────────────────────────────────────────────────── */
    case 'J': /* ED — Erase in Display */
        n = csi_param(t, 0, 0);
        if (n == 0) {
            /* Erase from cursor to end of screen */
            for (int col = t->cursor_col; col < t->cols; col++)
                clear_wide_cell(t, t->cursor_row, col);
            for (int row = t->cursor_row + 1; row < t->rows; row++)
                clear_row(t, row);
        } else if (n == 1) {
            /* Erase from start of screen to cursor */
            for (int row = 0; row < t->cursor_row; row++)
                clear_row(t, row);
            for (int col = 0; col <= t->cursor_col; col++)
                clear_wide_cell(t, t->cursor_row, col);
        } else if (n == 2 || n == 3) {
            /* Erase entire screen */
            for (int row = 0; row < t->rows; row++)
                clear_row(t, row);
        }
        break;

    case 'K': /* EL — Erase in Line */
        n = csi_param(t, 0, 0);
        if (n == 0) {
            /* Erase from cursor to end of line */
            for (int col = t->cursor_col; col < t->cols; col++)
                clear_wide_cell(t, t->cursor_row, col);
        } else if (n == 1) {
            /* Erase from start of line to cursor */
            for (int col = 0; col <= t->cursor_col; col++)
                clear_wide_cell(t, t->cursor_row, col);
        } else if (n == 2) {
            /* Erase entire line */
            clear_row(t, t->cursor_row);
        }
        break;

    /* ── Insert/Delete ────────────────────────────────────────── */
    case 'L': /* IL — Insert Lines */
    {
        n = csi_param(t, 0, 1);
        if (t->cursor_row >= t->scroll_top && t->cursor_row <= t->scroll_bottom) {
            int save_top = t->scroll_top;
            t->scroll_top = t->cursor_row;
            scroll_down(t, n);
            t->scroll_top = save_top;
        }
        break;
    }

    case 'M': /* DL — Delete Lines */
    {
        n = csi_param(t, 0, 1);
        if (t->cursor_row >= t->scroll_top && t->cursor_row <= t->scroll_bottom) {
            int save_top = t->scroll_top;
            t->scroll_top = t->cursor_row;
            scroll_up(t, n);
            t->scroll_top = save_top;
        }
        break;
    }

    case '@': /* ICH — Insert Characters */
    {
        n = csi_param(t, 0, 1);
        int row = t->cursor_row;
        int col = t->cursor_col;
        if (n > t->cols - col) n = t->cols - col;
        /* Shift right */
        for (int c = t->cols - 1; c >= col + n; c--) {
            *cell_at(t, row, c) = *cell_at(t, row, c - n);
        }
        /* Clear inserted cells */
        for (int c = col; c < col + n && c < t->cols; c++) {
            clear_cell(cell_at(t, row, c));
        }
        break;
    }

    case 'P': /* DCH — Delete Characters */
    {
        n = csi_param(t, 0, 1);
        int row = t->cursor_row;
        int col = t->cursor_col;
        if (n > t->cols - col) n = t->cols - col;
        /* Shift left */
        for (int c = col; c < t->cols - n; c++) {
            *cell_at(t, row, c) = *cell_at(t, row, c + n);
        }
        /* Clear vacated cells at end */
        for (int c = t->cols - n; c < t->cols; c++) {
            clear_cell(cell_at(t, row, c));
        }
        break;
    }

    case 'X': /* ECH — Erase Characters */
    {
        n = csi_param(t, 0, 1);
        for (int c = t->cursor_col; c < t->cursor_col + n && c < t->cols; c++) {
            clear_wide_cell(t, t->cursor_row, c);
        }
        break;
    }

    /* ── Scroll ───────────────────────────────────────────────── */
    case 'S': /* SU — Scroll Up */
        n = csi_param(t, 0, 1);
        scroll_up(t, n);
        break;

    case 'T': /* SD — Scroll Down */
        n = csi_param(t, 0, 1);
        scroll_down(t, n);
        break;

    /* ── Scroll Region ────────────────────────────────────────── */
    case 'r': /* DECSTBM — Set Scrolling Region */
    {
        int top = csi_param(t, 0, 1);
        int bot = csi_param(t, 1, t->rows);
        top = clamp(top - 1, 0, t->rows - 1);
        bot = clamp(bot - 1, 0, t->rows - 1);
        if (top < bot) {
            t->scroll_top = top;
            t->scroll_bottom = bot;
        }
        t->cursor_row = 0;
        t->cursor_col = 0;
        t->pending_wrap = 0;
        break;
    }

    /* ── SGR (decoration — we strip it) ───────────────────────── */
    case 'm': /* SGR — Select Graphic Rendition */
        /* Intentionally ignored — we strip all decoration. */
        break;

    /* ── Cursor visibility and other modes (ignored) ──────────── */
    case 'h': /* SM — Set Mode */
    case 'l': /* RM — Reset Mode */
        break;

    /* ── Device status / cursor position report (ignored) ─────── */
    case 'n':
        break;

    /* ── Tab clear ────────────────────────────────────────────── */
    case 'g': /* TBC — Tab Clear */
        n = csi_param(t, 0, 0);
        if (n == 0) {
            /* Clear tab stop at current column */
            if (t->cursor_col >= 0 && t->cursor_col < t->cols) {
                t->tab_stops[t->cursor_col / 8] &= (unsigned char)~(1 << (t->cursor_col % 8));
            }
        } else if (n == 3) {
            /* Clear all tab stops */
            memset(t->tab_stops, 0, (size_t)((t->cols + 7) / 8));
        }
        break;

    default:
        /* Unknown CSI sequence — silently ignore */
        break;
    }
}

/* ── ESC dispatch ─────────────────────────────────────────────────── */

static void dispatch_esc(ts_render_t *t, char ch) {
    switch (ch) {
    case '7': /* DECSC — Save Cursor */
        t->saved_cursor_row = t->cursor_row;
        t->saved_cursor_col = t->cursor_col;
        break;

    case '8': /* DECRC — Restore Cursor */
        t->cursor_row = clamp(t->saved_cursor_row, 0, t->rows - 1);
        t->cursor_col = clamp(t->saved_cursor_col, 0, t->cols - 1);
        t->pending_wrap = 0;
        break;

    case 'D': /* IND — Index (line feed) */
        do_linefeed(t);
        break;

    case 'E': /* NEL — Next Line */
        t->cursor_col = 0;
        do_linefeed(t);
        break;

    case 'M': /* RI — Reverse Index */
        do_reverse_linefeed(t);
        break;

    case 'H': /* HTS — Horizontal Tab Set */
        if (t->cursor_col >= 0 && t->cursor_col < t->cols) {
            t->tab_stops[t->cursor_col / 8] |= (unsigned char)(1 << (t->cursor_col % 8));
        }
        break;

    case 'c': /* RIS — Full Reset */
        ts_render_reset(t);
        break;

    default:
        /* Unknown ESC sequence — silently ignore */
        break;
    }
}

/* ── Ground state: process a single printable byte or control ─────── */

static void process_ground(ts_render_t *t, unsigned char ch) {
    if (ch < 0x20) {
        /* C0 control characters */
        switch (ch) {
        case '\n': /* LF */
            do_linefeed(t);
            break;

        case '\r': /* CR */
            t->cursor_col = 0;
            t->pending_wrap = 0;
            break;

        case '\t': /* HT — Horizontal Tab */
        {
            /* Move to next tab stop */
            int next_col = t->cursor_col + 1;
            while (next_col < t->cols && !is_tab_stop(t, next_col)) {
                next_col++;
            }
            if (next_col >= t->cols) next_col = t->cols - 1;
            t->cursor_col = next_col;
            t->pending_wrap = 0;
            break;
        }

        case '\b': /* BS — Backspace */
            if (t->cursor_col > 0) {
                t->cursor_col--;
                t->pending_wrap = 0;
            }
            break;

        case '\x07': /* BEL — ignore */
            break;

        case '\x0e': /* SO — Shift Out (ignored) */
        case '\x0f': /* SI — Shift In (ignored) */
            break;

        default:
            /* Other C0 controls — ignore */
            break;
        }
    } else if (ch == 0x7f) {
        /* DEL — ignore */
    } else if (ch >= 0x20 && ch <= 0x7e) {
        /* Printable ASCII */
        char c = (char)ch;
        put_char(t, &c, 1);
    } else if (ch >= 0xc2 && ch <= 0xf4) {
        /* UTF-8 leading byte */
        t->utf8_buf[0] = (char)ch;
        t->utf8_len = 1;
        if (ch <= 0xdf)      t->utf8_expect = 2;
        else if (ch <= 0xef) t->utf8_expect = 3;
        else                  t->utf8_expect = 4;
        /* Continuation bytes will be fed via process_utf8_cont */
    } else if (ch >= 0x80 && ch <= 0xbf) {
        /* Stray continuation byte — ignore */
    } else if (ch >= 0xc0 && ch <= 0xc1) {
        /* Overlong encoding — ignore */
    } else {
        /* Other high bytes (0xf5-0xff) — ignore */
    }
}

/* ── UTF-8 continuation byte processing ───────────────────────────── */

static int process_utf8_cont(ts_render_t *t, unsigned char ch) {
    if (t->utf8_len == 0) return 0; /* not in multi-byte sequence */

    if ((ch & 0xc0) != 0x80) {
        /* Not a continuation byte — abort sequence, reprocess byte */
        t->utf8_len = 0;
        t->utf8_expect = 0;
        return 0; /* caller should reprocess ch */
    }

    t->utf8_buf[t->utf8_len++] = (char)ch;
    if (t->utf8_len == t->utf8_expect) {
        /* Complete UTF-8 character */
        put_char(t, t->utf8_buf, t->utf8_len);
        t->utf8_len = 0;
        t->utf8_expect = 0;
    }
    return 1; /* byte consumed */
}

/* ── State machine: process one byte ──────────────────────────────── */

static void process_byte(ts_render_t *t, unsigned char ch) {
    /* UTF-8 multi-byte continuation (only in ground state) */
    if (t->state == STATE_GROUND && t->utf8_len > 0) {
        if (process_utf8_cont(t, ch))
            return;
        /* else: not a continuation byte, fall through to reprocess */
    }

    /* ESC always transitions (even mid-sequence) */
    if (ch == 0x1b) {
        t->state = STATE_ESC;
        t->intermediate = 0;
        return;
    }

    switch (t->state) {
    case STATE_GROUND:
        process_ground(t, ch);
        break;

    case STATE_ESC:
        if (ch == '[') {
            /* CSI introducer */
            t->state = STATE_CSI_PARAM;
            t->param_count = 0;
            t->param_val = 0;
            t->param_has_val = 0;
            t->intermediate = 0;
        } else if (ch == ']') {
            /* OSC introducer */
            t->state = STATE_OSC;
        } else if (ch == 'P') {
            /* DCS introducer */
            t->state = STATE_DCS;
        } else if (ch >= 0x20 && ch <= 0x2f) {
            /* Intermediate byte after ESC */
            t->state = STATE_ESC_INTER;
            t->intermediate = (char)ch;
        } else if (ch >= 0x30 && ch <= 0x7e) {
            /* Final byte — dispatch ESC sequence */
            dispatch_esc(t, (char)ch);
            t->state = STATE_GROUND;
        } else {
            /* Invalid — back to ground */
            t->state = STATE_GROUND;
        }
        break;

    case STATE_ESC_INTER:
        if (ch >= 0x20 && ch <= 0x2f) {
            /* More intermediate bytes — absorb */
        } else if (ch >= 0x30 && ch <= 0x7e) {
            /* Final byte — ignore ESC+intermediate sequences
             * (e.g. ESC ( B for character sets) */
            t->state = STATE_GROUND;
        } else {
            t->state = STATE_GROUND;
        }
        break;

    case STATE_CSI_PARAM:
        if (ch >= '0' && ch <= '9') {
            t->param_val = (t->param_val > 99999) ? 99999 : t->param_val * 10 + (ch - '0');
            t->param_has_val = 1;
        } else if (ch == ';') {
            /* Parameter separator */
            if (t->param_count < NBS_TS_RENDER_MAX_PARAMS) {
                t->params[t->param_count++] = t->param_has_val ? t->param_val : 0;
            }
            t->param_val = 0;
            t->param_has_val = 0;
        } else if (ch == '?' || ch == '>' || ch == '!') {
            /* Private mode prefix — record as intermediate */
            t->intermediate = (char)ch;
        } else if (ch >= 0x20 && ch <= 0x2f) {
            /* Intermediate byte */
            t->intermediate = (char)ch;
            t->state = STATE_CSI_INTER;
        } else if (ch >= 0x40 && ch <= 0x7e) {
            /* Final byte — dispatch */
            dispatch_csi(t, (char)ch);
            t->state = STATE_GROUND;
        } else {
            /* Invalid — abort CSI, back to ground */
            t->state = STATE_GROUND;
        }
        break;

    case STATE_CSI_INTER:
        if (ch >= 0x20 && ch <= 0x2f) {
            /* More intermediate bytes — absorb */
        } else if (ch >= 0x40 && ch <= 0x7e) {
            /* Final byte — dispatch */
            dispatch_csi(t, (char)ch);
            t->state = STATE_GROUND;
        } else {
            t->state = STATE_GROUND;
        }
        break;

    case STATE_OSC:
        if (ch == 0x07) {
            /* BEL terminates OSC */
            t->state = STATE_GROUND;
        } else if (ch == 0x1b) {
            t->state = STATE_OSC_ESC;
        }
        /* else: absorb OSC content */
        break;

    case STATE_OSC_ESC:
        if (ch == '\\') {
            /* ST (ESC \) terminates OSC */
            t->state = STATE_GROUND;
        } else {
            /* Not ST — ESC starts new sequence */
            t->state = STATE_ESC;
            /* Reprocess ch in ESC state */
            process_byte(t, ch);
        }
        break;

    case STATE_DCS:
        if (ch == 0x1b) {
            t->state = STATE_DCS_ESC;
        }
        /* else: absorb DCS content */
        break;

    case STATE_DCS_ESC:
        if (ch == '\\') {
            /* ST terminates DCS */
            t->state = STATE_GROUND;
        } else {
            t->state = STATE_DCS;
        }
        break;
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

ts_render_t *ts_render_create(int rows, int cols) {
    ASSERT_MSG(rows > 0, "ts_render_create: rows must be positive, got %d", rows);
    ASSERT_MSG(cols > 0, "ts_render_create: cols must be positive, got %d", cols);

    ts_render_t *t = calloc(1, sizeof(ts_render_t));
    if (!t) return NULL;

    t->rows = rows;
    t->cols = cols;
    t->cells = calloc((size_t)(rows * cols), sizeof(ts_render_cell_t));
    if (!t->cells) {
        free(t);
        return NULL;
    }

    int tab_bytes = (cols + 7) / 8;
    t->tab_stops = calloc(1, (size_t)tab_bytes);
    if (!t->tab_stops) {
        free(t->cells);
        free(t);
        return NULL;
    }

    ts_render_reset(t);
    return t;
}

void ts_render_destroy(ts_render_t *t) {
    if (!t) return;
    free(t->tab_stops);
    free(t->cells);
    free(t);
}

void ts_render_feed(ts_render_t *t, const char *data, size_t len) {
    ASSERT_MSG(t != NULL, "ts_render_feed: t must be non-NULL");
    ASSERT_MSG(data != NULL || len == 0,
               "ts_render_feed: data is NULL with non-zero len %zu", len);
    for (size_t i = 0; i < len; i++) {
        process_byte(t, (unsigned char)data[i]);
    }
}

char *ts_render_snapshot(const ts_render_t *t) {
    ASSERT_MSG(t != NULL, "ts_render_snapshot: t must be non-NULL");

    /* Worst case: each cell is 4 bytes + newline per row + NUL */
    size_t buf_size = (size_t)(t->rows * (t->cols * NBS_TS_RENDER_CELL_BYTES + 1)) + 1;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;

    size_t pos = 0;
    for (int row = 0; row < t->rows; row++) {
        /* Find last non-empty cell to trim trailing spaces */
        int last_col = -1;
        for (int col = t->cols - 1; col >= 0; col--) {
            const ts_render_cell_t *c = cell_at_const(t, row, col);
            if (CELL_IS_CONTINUATION(c)) { last_col = col; break; }
            if (c->len > 0) {
                /* Check if it's a space */
                if (c->len == 1 && c->ch[0] == ' ') continue;
                last_col = col;
                break;
            }
        }

        /* Collect logical characters for bidi reordering */
        int char_count = 0;
        int col_map[t->cols]; /* col_map[char_idx] = column of that char */
        uint32_t codepoints[t->cols];

        for (int col = 0; col <= last_col; col++) {
            const ts_render_cell_t *c = cell_at_const(t, row, col);
            if (CELL_IS_CONTINUATION(c)) continue;
            col_map[char_count] = col;
            if (c->len > 0) {
                codepoints[char_count] = utf8_to_codepoint(c->ch, c->len > 4 ? 4 : c->len);
            } else {
                codepoints[char_count] = 0x0020; /* empty cell = space */
            }
            char_count++;
        }

        /* Apply bidi reordering with levels for mirroring */
        int visual_map[t->cols];
        int bidi_levels[t->cols];
        if (char_count > 0) {
            nbs_ts_bidi_reorder_with_levels(codepoints, char_count,
                                            visual_map, bidi_levels, 0);
        }

        /* Write cells in visual order, with bracket mirroring */
        for (int vi = 0; vi < char_count; vi++) {
            int li = visual_map[vi]; /* logical index */
            int col = col_map[li];
            const ts_render_cell_t *c = cell_at_const(t, row, col);
            if (c->len > 0) {
                /* Check if this char needs mirroring (odd bidi level) */
                if (bidi_levels[li] & 1) {
                    uint32_t cp = codepoints[li];
                    uint32_t mirrored = nbs_ts_bidi_mirror(cp);
                    if (mirrored != cp && cp < 0x80) {
                        /* ASCII mirror — single byte replacement */
                        ASSERT_MSG(pos + 1 < buf_size,
                                   "ts_render_snapshot: buffer overflow at row=%d col=%d", row, col);
                        buf[pos++] = (char)mirrored;
                    } else if (mirrored != cp) {
                        /* Non-ASCII mirror — encode as UTF-8 */
                        char mirror_buf[4];
                        int mlen = 0;
                        if (mirrored < 0x80) { mirror_buf[0] = (char)mirrored; mlen = 1; }
                        else if (mirrored < 0x800) { mirror_buf[0] = (char)(0xC0 | (mirrored >> 6)); mirror_buf[1] = (char)(0x80 | (mirrored & 0x3F)); mlen = 2; }
                        else if (mirrored < 0x10000) { mirror_buf[0] = (char)(0xE0 | (mirrored >> 12)); mirror_buf[1] = (char)(0x80 | ((mirrored >> 6) & 0x3F)); mirror_buf[2] = (char)(0x80 | (mirrored & 0x3F)); mlen = 3; }
                        else { mirror_buf[0] = (char)(0xF0 | (mirrored >> 18)); mirror_buf[1] = (char)(0x80 | ((mirrored >> 12) & 0x3F)); mirror_buf[2] = (char)(0x80 | ((mirrored >> 6) & 0x3F)); mirror_buf[3] = (char)(0x80 | (mirrored & 0x3F)); mlen = 4; }
                        ASSERT_MSG(pos + (size_t)mlen < buf_size,
                                   "ts_render_snapshot: buffer overflow at row=%d col=%d", row, col);
                        memcpy(buf + pos, mirror_buf, (size_t)mlen);
                        pos += (size_t)mlen;
                    } else {
                        /* No mirror — output as-is */
                        ASSERT_MSG(pos + (size_t)c->len < buf_size,
                                   "ts_render_snapshot: buffer overflow at row=%d col=%d", row, col);
                        memcpy(buf + pos, c->ch, (size_t)c->len);
                        pos += (size_t)c->len;
                    }
                } else {
                    ASSERT_MSG(pos + (size_t)c->len < buf_size,
                               "ts_render_snapshot: buffer overflow at row=%d col=%d", row, col);
                    memcpy(buf + pos, c->ch, (size_t)c->len);
                    pos += (size_t)c->len;
                }
            } else {
                /* Empty cell rendered as space */
                ASSERT_MSG(pos + 1 < buf_size,
                           "ts_render_snapshot: buffer overflow (space) at row=%d col=%d", row, col);
                buf[pos++] = ' ';
            }
        }

        /* Add newline (except after last row if it's empty) */
        if (row < t->rows - 1 || last_col >= 0) {
            buf[pos++] = '\n';
        }
    }

    buf[pos] = '\0';

    /* Trim trailing blank lines (lines that are just newlines) */
    while (pos > 0 && buf[pos - 1] == '\n') {
        /* Check if the line before is also empty */
        if (pos >= 2 && buf[pos - 2] == '\n') {
            pos--;
            buf[pos] = '\0';
        } else {
            break;
        }
    }

    return buf;
}

void ts_render_reset(ts_render_t *t) {
    ASSERT_MSG(t != NULL, "ts_render_reset: t must be non-NULL");

    for (int row = 0; row < t->rows; row++) {
        clear_row(t, row);
    }

    t->cursor_row = 0;
    t->cursor_col = 0;
    t->state = STATE_GROUND;
    t->param_count = 0;
    t->param_val = 0;
    t->param_has_val = 0;
    t->intermediate = 0;
    t->scroll_top = 0;
    t->scroll_bottom = t->rows - 1;
    t->pending_wrap = 0;
    t->saved_cursor_row = 0;
    t->saved_cursor_col = 0;
    t->utf8_len = 0;
    t->utf8_expect = 0;

    init_tab_stops(t);
}
