/*
 * md_viewport.c — Viewport and display implementation.
 *
 * Manages scroll/pan state and renders visible lines with styled spans
 * using nbs_style_fstart/freset. Draws a status bar on the last row.
 */

#define _POSIX_C_SOURCE 200809L

#include "md_viewport.h"
#include "md_terminal.h"
#include "md_parse.h"
#include "md_style.h"
#include "md_render.h"
#include "../nbs-common/nbs_term_attr.h"
#include "../nbs-ts-render/nbs_ts_wcwidth.h"
#include "../nbs-ts-render/nbs_ts_bidi.h"
#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --- Internal helpers --- */

/*
 * decode_utf8 — Decode one UTF-8 character starting at text[pos].
 * Writes the codepoint to *out_cp and returns the number of bytes consumed.
 * On invalid input returns 1 and sets *out_cp to the raw byte.
 */
static int decode_utf8(const char *text, int len, int pos, uint32_t *out_cp) {
    unsigned char ch = (unsigned char)text[pos];
    int blen = 1;
    uint32_t cp;

    if (ch < 0x80) {
        *out_cp = ch;
        return 1;
    } else if (ch >= 0xF0) {
        blen = 4; cp = ch & 0x07;
    } else if (ch >= 0xE0) {
        blen = 3; cp = ch & 0x0F;
    } else if (ch >= 0xC0) {
        blen = 2; cp = ch & 0x1F;
    } else {
        /* Continuation byte without leading byte — treat as raw byte */
        *out_cp = ch;
        return 1;
    }

    if (pos + blen > len) {
        *out_cp = ch;
        return 1;
    }
    for (int b = 1; b < blen; b++)
        cp = (cp << 6) | ((unsigned char)text[pos + b] & 0x3F);

    *out_cp = cp;
    return blen;
}

/*
 * max_scroll — Compute the maximum valid scroll offset.
 * Returns 0 if the document fits entirely on screen.
 */
static int max_scroll(const md_view_state_t *vs) {
    int m = vs->total_lines - vs->visible_rows;
    return (m > 0) ? m : 0;
}

/*
 * clamp_scroll — Ensure scroll_offset is within [0, max_scroll].
 */
static void clamp_scroll(md_view_state_t *vs) {
    int m = max_scroll(vs);
    if (vs->scroll_offset > m) vs->scroll_offset = m;
    if (vs->scroll_offset < 0) vs->scroll_offset = 0;
}

/* --- Public API --- */

void md_viewport_init(md_view_state_t *vs, int rows, int cols, int total_lines) {
    ASSERT_MSG(vs != NULL, "md_viewport_init: vs is NULL");
    ASSERT_MSG(rows > 0, "md_viewport_init: rows must be positive, got %d", rows);
    ASSERT_MSG(cols > 0, "md_viewport_init: cols must be positive, got %d", cols);

    vs->visible_rows = rows - 1;  /* status bar takes 1 row */
    if (vs->visible_rows < 1) vs->visible_rows = 1;
    vs->terminal_cols = cols;
    vs->total_lines = total_lines;
    vs->resize_pending = 0;

    /* Preserve existing scroll_offset if struct was already in use,
     * but clamp it to the new bounds. h_offset resets to 0. */
    vs->h_offset = 0;
    clamp_scroll(vs);
}

void md_viewport_scroll_up(md_view_state_t *vs, int n) {
    vs->scroll_offset -= n;
    clamp_scroll(vs);
}

void md_viewport_scroll_down(md_view_state_t *vs, int n) {
    vs->scroll_offset += n;
    clamp_scroll(vs);
}

void md_viewport_page_up(md_view_state_t *vs) {
    int step = vs->visible_rows - 1;
    if (step < 1) step = 1;
    vs->scroll_offset -= step;
    clamp_scroll(vs);
}

void md_viewport_page_down(md_view_state_t *vs) {
    int step = vs->visible_rows - 1;
    if (step < 1) step = 1;
    vs->scroll_offset += step;
    clamp_scroll(vs);
}

void md_viewport_home(md_view_state_t *vs) {
    vs->scroll_offset = 0;
}

void md_viewport_end(md_view_state_t *vs) {
    vs->scroll_offset = max_scroll(vs);
}

void md_viewport_pan_right(md_view_state_t *vs) {
    vs->h_offset += 4;
}

void md_viewport_pan_left(md_view_state_t *vs) {
    vs->h_offset -= 4;
    if (vs->h_offset < 0) vs->h_offset = 0;
}

void md_viewport_draw(md_view_state_t *vs, md_layout_t *layout) {
    ASSERT_MSG(vs != NULL, "md_viewport_draw: vs is NULL");
    ASSERT_MSG(layout != NULL, "md_viewport_draw: layout is NULL");

    FILE *out = stdout;

    /* Re-query terminal size to guard against stale dimensions */
    {
        int cur_rows, cur_cols;
        if (md_terminal_get_size(&cur_rows, &cur_cols) == 0) {
            if (cur_cols != vs->terminal_cols || cur_rows - 1 != vs->visible_rows) {
                vs->terminal_cols = cur_cols;
                vs->visible_rows = cur_rows - 1;
                if (vs->visible_rows < 1) vs->visible_rows = 1;
            }
        }
    }

    /* Move cursor to home position */
    fputs("\033[H", out);

    for (int row = 0; row < vs->visible_rows; row++) {
        int line_idx = vs->scroll_offset + row;

        if (line_idx >= vs->total_lines || line_idx >= layout->line_count) {
            /* Past end of document: empty line */
            fputs("\033[K", out);
            if (row < vs->visible_rows - 1) {
                fputs("\r\n", out);
            }
            continue;
        }

        md_display_line_t *dl = &layout->lines[line_idx];

        /* Determine horizontal offset: only wide lines support h-pan */
        int h_off = 0;
        if (dl->is_wide_line && vs->h_offset > 0) {
            h_off = vs->h_offset;
        }

        /*
         * BiDi-aware rendering: collect all codepoints from every span
         * on this display line, run UAX #9 reordering, then output
         * characters in visual order with the correct per-character style.
         *
         * For terminals that handle BiDi natively, the reorder is a no-op
         * on LTR-only text. For terminals that do not, this ensures
         * correct visual ordering of mixed LTR/RTL content.
         */

        /* --- Pass 1: count total codepoints across all spans --- */
        int total_cps = 0;
        for (int s = 0; s < dl->span_count; s++) {
            const char *text = dl->spans[s].text;
            int tlen = (int)strlen(text);
            int pos = 0;
            while (pos < tlen) {
                uint32_t cp;
                pos += decode_utf8(text, tlen, pos, &cp);
                total_cps++;
            }
        }

        if (total_cps == 0) {
            /* No text to render — skip to line clear */
            goto line_done;
        }

        /* --- Pass 2: fill codepoint + per-char metadata arrays --- */
        uint32_t *cps = malloc((size_t)total_cps * sizeof(uint32_t));
        int *span_idx = malloc((size_t)total_cps * sizeof(int));
        /* byte_off[i] = starting byte offset within its span's text */
        int *byte_off = malloc((size_t)total_cps * sizeof(int));
        /* byte_len[i] = byte length of this character's UTF-8 encoding */
        int *byte_len_arr = malloc((size_t)total_cps * sizeof(int));
        int *visual_map = malloc((size_t)total_cps * sizeof(int));

        if (!cps || !span_idx || !byte_off || !byte_len_arr || !visual_map) {
            free(cps); free(span_idx); free(byte_off);
            free(byte_len_arr); free(visual_map);
            goto line_done;
        }

        int ci = 0;
        for (int s = 0; s < dl->span_count; s++) {
            const char *text = dl->spans[s].text;
            int tlen = (int)strlen(text);
            int pos = 0;
            while (pos < tlen) {
                uint32_t cp;
                int blen = decode_utf8(text, tlen, pos, &cp);
                cps[ci] = cp;
                span_idx[ci] = s;
                byte_off[ci] = pos;
                byte_len_arr[ci] = blen;
                ci++;
                pos += blen;
            }
        }

        /* --- Pass 3: BiDi reorder (logical -> visual) --- */
        nbs_ts_bidi_reorder(cps, total_cps, visual_map, 0 /* auto-detect */);

        /* --- Pass 4: output in visual order with h_off applied --- */
        int col = 0;
        int prev_span = -1;
        for (int v = 0; v < total_cps; v++) {
            int li = visual_map[v];           /* logical index */
            uint32_t cp = cps[li];
            int w = nbs_ts_wcwidth((int)cp);
            if (w < 0) w = 0;

            /* Apply horizontal offset: skip characters before h_off */
            if (col + w <= h_off) {
                col += w;
                continue;
            }

            /* Truncate at right edge of terminal */
            if (col - h_off >= vs->terminal_cols) {
                col += w;
                continue;
            }

            /* Emit style change when the source span changes */
            int si = span_idx[li];
            if (si != prev_span) {
                if (prev_span >= 0) {
                    nbs_style_freset(out);
                }
                nbs_style_fstart(&dl->spans[si].style, out);
                prev_span = si;
            }

            /* Write the UTF-8 bytes for this character */
            const char *text = dl->spans[si].text;
            fwrite(text + byte_off[li], 1, (size_t)byte_len_arr[li], out);
            col += w;
        }
        if (prev_span >= 0) {
            nbs_style_freset(out);
        }

        free(cps);
        free(span_idx);
        free(byte_off);
        free(byte_len_arr);
        free(visual_map);

line_done:

        /* Clear to end of line */
        fputs("\033[K", out);

        if (row < vs->visible_rows - 1) {
            fputs("\r\n", out);
        }
    }

    /* --- Status bar on the last terminal row --- */
    fputs("\r\n", out);
    nbs_style_fstart(&MD_STYLE_STATUS_BAR, out);

    int total = vs->total_lines;
    int pct = 0;
    if (total > 0) {
        int bottom = vs->scroll_offset + vs->visible_rows;
        if (bottom > total) bottom = total;
        pct = (bottom * 100) / total;
    }

    char status[256];
    int slen = snprintf(status, sizeof(status),
                        " Line %d/%d  %d%%",
                        vs->scroll_offset + 1, total, pct);

    /* Write status text and pad to full terminal width */
    fwrite(status, 1, (size_t)slen, out);
    for (int i = slen; i < vs->terminal_cols; i++) {
        fputc(' ', out);
    }

    nbs_style_freset(out);
    fflush(out);
}

void md_viewport_draw_help(md_view_state_t *vs) {
    static const char *help_md =
        "## nbs-md-viewer\n"
        "\n"
        "| Key | Action |\n"
        "|-----|--------|\n"
        "| Up / Down | Scroll one line |\n"
        "| Page Up / Down | Scroll one page |\n"
        "| Home | Jump to top |\n"
        "| End | Jump to bottom |\n"
        "| Left / Right | Pan wide tables and code |\n"
        "| h / ? | This help screen |\n"
        "| q | Quit |\n"
        "\n"
        "*On Mac: use Fn + Arrow for Page/Home/End*\n"
        "\n"
        "---\n"
        "\n"
        "*Press Enter to return*\n";

    md_block_node_t *doc = md_parse(help_md);
    md_layout_t *layout = md_render(doc, vs->terminal_cols);

    md_view_state_t help_vs;
    memset(&help_vs, 0, sizeof(help_vs));
    md_viewport_init(&help_vs, vs->visible_rows + 1, vs->terminal_cols, layout->line_count);
    md_viewport_draw(&help_vs, layout);

    md_layout_destroy(layout);
    md_block_destroy(doc);
}
