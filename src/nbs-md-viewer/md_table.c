/*
 * md_table.c — Table layout and Unicode box drawing.
 *
 * Renders a table AST node into display lines with:
 * - Column width measurement (max cell width + 1 padding each side)
 * - Unicode box drawing (single-line borders, double-horizontal header separator)
 * - LEFT/CENTRE/RIGHT alignment
 * - Truncation at terminal width
 */

#include "md_table.h"
#include "md_style.h"
#include "../nbs-common/nbs_assert.h"
#include "../nbs-ts-render/nbs_ts_wcwidth.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── UTF-8 helpers (shared with md_render.c, local copies) ──── */

static int utf8_decode_t(const char *s, int len, uint32_t *cp) {
    if (len <= 0) { *cp = 0; return 0; }
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    else if ((c & 0xE0) == 0xC0) {
        if (len < 2) { *cp = 0xFFFD; return 1; }
        *cp = ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    } else if ((c & 0xF0) == 0xE0) {
        if (len < 3) { *cp = 0xFFFD; return 1; }
        *cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    } else if ((c & 0xF8) == 0xF0) {
        if (len < 4) { *cp = 0xFFFD; return 1; }
        *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

static int utf8_display_width_t(const char *s, int len) {
    int w = 0, i = 0;
    while (i < len) {
        uint32_t cp;
        int b = utf8_decode_t(s + i, len - i, &cp);
        if (b == 0) break;
        int cw = nbs_ts_wcwidth(cp);
        if (cw > 0) w += cw;
        i += b;
    }
    return w;
}

/* ── box drawing characters ──────────────────────────────────────── */

/* Single-line box drawing */
#define BOX_TL  "\xe2\x94\x8c"  /* ┌ U+250C */
#define BOX_TR  "\xe2\x94\x90"  /* ┐ U+2510 */
#define BOX_BL  "\xe2\x94\x94"  /* └ U+2514 */
#define BOX_BR  "\xe2\x94\x98"  /* ┘ U+2518 */
#define BOX_H   "\xe2\x94\x80"  /* ─ U+2500 */
#define BOX_V   "\xe2\x94\x82"  /* │ U+2502 */
#define BOX_TD  "\xe2\x94\xac"  /* ┬ U+252C */
#define BOX_TU  "\xe2\x94\xb4"  /* ┴ U+2534 */
#define BOX_TRt "\xe2\x94\x9c"  /* ├ U+251C */
#define BOX_TLt "\xe2\x94\xa4"  /* ┤ U+2524 */
#define BOX_X   "\xe2\x94\xbc"  /* ┼ U+253C */

/* Double-horizontal for header separator */
#define BOX_DH  "\xe2\x95\x90"  /* ═ U+2550 */
#define BOX_DTD "\xe2\x95\xa4"  /* ╤ U+2564 */
#define BOX_DTU "\xe2\x95\xa7"  /* ╧ U+2567 */
#define BOX_DX  "\xe2\x95\xaa"  /* ╪ U+256A */
#define BOX_DL  "\xe2\x95\x9e"  /* ╞ U+255E */
#define BOX_DR  "\xe2\x95\xa1"  /* ╡ U+2561 */

/* ── span add helper ─────────────────────────────────────────────── */

static void tbl_add_span(md_display_line_t *dl, const char *text, int text_len,
                          nbs_style_t style, int display_width) {
    int n = dl->span_count;
    dl->spans = realloc(dl->spans, (size_t)(n + 1) * sizeof(md_span_t));
    char *t = malloc((size_t)text_len + 1);
    memcpy(t, text, (size_t)text_len);
    t[text_len] = '\0';
    dl->spans[n].text = t;
    dl->spans[n].style = style;
    dl->spans[n].width = display_width;
    dl->span_count = n + 1;
    dl->display_width += display_width;
}

/* ── cell text extraction ────────────────────────────────────────── */

/* Get plain text content from inline nodes of a cell */
static char *cell_text(md_block_node_t *cell) {
    if (!cell || !cell->inlines) return strdup("");

    char *buf = NULL;
    int len = 0;
    int cap = 0;

    md_inline_node_t *inl = cell->inlines;
    while (inl) {
        const char *t = inl->text;
        if (t) {
            int tl = (int)strlen(t);
            if (len + tl >= cap) {
                cap = (len + tl) * 2 + 16;
                buf = realloc(buf, (size_t)cap);
            }
            memcpy(buf + len, t, (size_t)tl);
            len += tl;
        }
        /* recurse into children */
        if (inl->children) {
            md_inline_node_t *ch = inl->children;
            while (ch) {
                if (ch->text) {
                    int tl = (int)strlen(ch->text);
                    if (len + tl >= cap) {
                        cap = (len + tl) * 2 + 16;
                        buf = realloc(buf, (size_t)cap);
                    }
                    memcpy(buf + len, ch->text, (size_t)tl);
                    len += tl;
                }
                ch = ch->next;
            }
        }
        inl = inl->next;
    }

    if (!buf) return strdup("");
    buf[len] = '\0';
    return buf;
}

/* ── main table render ───────────────────────────────────────────── */

void md_table_render(md_layout_t *layout, md_block_node_t *table,
                     int terminal_width, int block_id) {
    ASSERT_MSG(table != NULL, "md_table_render: table is NULL");
    ASSERT_MSG(table->type == MD_BLOCK_TABLE, "md_table_render: not a table node");
    (void)terminal_width; /* TODO: truncation at terminal width */

    int ncols = table->col_count;
    if (ncols <= 0) return;

    /* Count rows and collect cell texts */
    int nrows = 0;
    md_block_node_t *row = table->children;
    while (row) { nrows++; row = row->next; }
    if (nrows == 0) return;

    /* Allocate cell text array [row][col] */
    char ***cells = calloc((size_t)nrows, sizeof(char **));
    int *is_header = calloc((size_t)nrows, sizeof(int));
    int r = 0;
    row = table->children;
    while (row) {
        cells[r] = calloc((size_t)ncols, sizeof(char *));
        is_header[r] = row->is_header;
        md_block_node_t *cell = row->children;
        int c = 0;
        while (cell && c < ncols) {
            cells[r][c] = cell_text(cell);
            cell = cell->next;
            c++;
        }
        while (c < ncols) {
            cells[r][c] = strdup("");
            c++;
        }
        r++;
        row = row->next;
    }

    /* Measure column widths */
    int *col_widths = calloc((size_t)ncols, sizeof(int));
    for (int c = 0; c < ncols; c++) {
        for (int ri = 0; ri < nrows; ri++) {
            int w = utf8_display_width_t(cells[ri][c], (int)strlen(cells[ri][c]));
            if (w > col_widths[c]) col_widths[c] = w;
        }
        col_widths[c] += 2; /* 1 padding each side */
    }

    /* Helper: build a horizontal border line */
    /* left, middle, right are 3-byte UTF-8 strings; fill is 3-byte */
    #define BUILD_HBORDER(left, fill, mid, right) do { \
        md_display_line_t hline; \
        memset(&hline, 0, sizeof(hline)); \
        hline.source_block = block_id; \
        hline.is_wide_line = 1; \
        char *hbuf = NULL; \
        int hlen = 0, hcap = 0; \
        /* left corner */ \
        { int need = hlen + 3 + 1; if (need > hcap) { hcap = need * 2; hbuf = realloc(hbuf, (size_t)hcap); } \
          memcpy(hbuf + hlen, left, 3); hlen += 3; } \
        for (int ci = 0; ci < ncols; ci++) { \
            /* fill chars for column width */ \
            for (int fi = 0; fi < col_widths[ci]; fi++) { \
                int need = hlen + 3 + 1; if (need > hcap) { hcap = need * 2; hbuf = realloc(hbuf, (size_t)hcap); } \
                memcpy(hbuf + hlen, fill, 3); hlen += 3; \
            } \
            /* middle or right corner */ \
            if (ci < ncols - 1) { \
                int need = hlen + 3 + 1; if (need > hcap) { hcap = need * 2; hbuf = realloc(hbuf, (size_t)hcap); } \
                memcpy(hbuf + hlen, mid, 3); hlen += 3; \
            } else { \
                int need = hlen + 3 + 1; if (need > hcap) { hcap = need * 2; hbuf = realloc(hbuf, (size_t)hcap); } \
                memcpy(hbuf + hlen, right, 3); hlen += 3; \
            } \
        } \
        hbuf[hlen] = '\0'; \
        int tw = 1; /* left corner */ \
        for (int ci = 0; ci < ncols; ci++) tw += col_widths[ci] + 1; \
        tbl_add_span(&hline, hbuf, hlen, MD_STYLE_TABLE_BORDER, tw); \
        free(hbuf); \
        md_layout_add_line(layout, &hline); \
    } while(0)

    /* Top border: ┌─┬─┐ */
    BUILD_HBORDER(BOX_TL, BOX_H, BOX_TD, BOX_TR);

    /* Data rows */
    for (int ri = 0; ri < nrows; ri++) {
        md_display_line_t dline;
        memset(&dline, 0, sizeof(dline));
        dline.source_block = block_id;
        dline.is_wide_line = 1;

        nbs_style_t text_style = is_header[ri] ? MD_STYLE_TABLE_HEADER : MD_STYLE_TABLE_CELL;

        for (int ci = 0; ci < ncols; ci++) {
            /* left border or separator */
            tbl_add_span(&dline, BOX_V, 3, MD_STYLE_TABLE_BORDER, 1);

            /* Cell content with alignment */
            int cw = col_widths[ci];
            char *ct = cells[ri][ci];
            int ct_width = utf8_display_width_t(ct, (int)strlen(ct));
            int pad = cw - ct_width;
            if (pad < 0) pad = 0;

            md_align_t align = MD_ALIGN_LEFT;
            if (table->col_align && ci < ncols) {
                align = table->col_align[ci];
            }

            /* col_widths[ci] = max_content_width + 2 (1 space padding each side).
             * pad = col_widths[ci] - ct_width = available space around content.
             * We need: left_pad + ct_width + right_pad = col_widths[ci].
             * So: left_pad + right_pad = pad. */
            int left_pad, right_pad;
            switch (align) {
                case MD_ALIGN_CENTRE:
                    left_pad = pad / 2;
                    right_pad = pad - pad / 2;
                    break;
                case MD_ALIGN_RIGHT:
                    left_pad = pad - 1;
                    right_pad = 1;
                    break;
                default: /* LEFT */
                    left_pad = 1;
                    right_pad = pad - 1;
                    break;
            }

            if (left_pad > 0) {
                char *sp = calloc(1, (size_t)left_pad + 1);
                memset(sp, ' ', (size_t)left_pad);
                tbl_add_span(&dline, sp, left_pad, text_style, left_pad);
                free(sp);
            }

            int ct_len = (int)strlen(ct);
            if (ct_len > 0) {
                /* Apply header or body bg band */
                nbs_style_t cell_style = text_style;
                if (is_header[ri] && MD_STYLE_TABLE_HEADER.bg != NBS_COLOUR_NONE) {
                    cell_style = MD_STYLE_TABLE_HEADER;
                }
                tbl_add_span(&dline, ct, ct_len, cell_style, ct_width);
            }

            if (right_pad > 0) {
                char *sp = calloc(1, (size_t)right_pad + 1);
                memset(sp, ' ', (size_t)right_pad);
                tbl_add_span(&dline, sp, right_pad, text_style, right_pad);
                free(sp);
            }
        }

        /* right border */
        tbl_add_span(&dline, BOX_V, 3, MD_STYLE_TABLE_BORDER, 1);
        md_layout_add_line(layout, &dline);

        /* Header separator after first header row: ╞═╪═╡ */
        if (is_header[ri]) {
            BUILD_HBORDER(BOX_DL, BOX_DH, BOX_DX, BOX_DR);
        }
    }

    /* Bottom border: └─┴─┘ */
    BUILD_HBORDER(BOX_BL, BOX_H, BOX_TU, BOX_BR);

    #undef BUILD_HBORDER

    /* Free cell texts */
    for (int ri = 0; ri < nrows; ri++) {
        for (int ci = 0; ci < ncols; ci++) free(cells[ri][ci]);
        free(cells[ri]);
    }
    free(cells);
    free(is_header);
    free(col_widths);
}
