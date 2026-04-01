/*
 * md_render.c — AST to styled display lines.
 *
 * Walks the AST and produces display lines with styled spans.
 * Handles paragraph reflow, heading rendering, horizontal rules,
 * list rendering with bullets/numbers, code fences with borders,
 * tables (via md_table), blockquotes, and block spacing.
 */

#include "md_render.h"
#include "md_style.h"
#include "md_table.h"
#include "md_highlight.h"
#include "../nbs-common/nbs_assert.h"
#include "../nbs-ts-render/nbs_ts_wcwidth.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── layout management ───────────────────────────────────────────── */

void md_layout_add_line(md_layout_t *layout, md_display_line_t *line) {
    int n = layout->line_count;
    layout->lines = realloc(layout->lines, (size_t)(n + 1) * sizeof(md_display_line_t));
    ASSERT_MSG(layout->lines != NULL, "md_layout_add_line: realloc failed");
    layout->lines[n] = *line;
    layout->line_count = n + 1;
    if (line->display_width > layout->max_width) {
        layout->max_width = line->display_width;
    }
}

void md_layout_add_blank(md_layout_t *layout, int block_id) {
    md_display_line_t blank;
    memset(&blank, 0, sizeof(blank));
    blank.source_block = block_id;
    md_layout_add_line(layout, &blank);
}

void md_layout_destroy(md_layout_t *layout) {
    if (!layout) return;
    for (int i = 0; i < layout->line_count; i++) {
        md_display_line_t *dl = &layout->lines[i];
        for (int j = 0; j < dl->span_count; j++) {
            free(dl->spans[j].text);
        }
        free(dl->spans);
    }
    free(layout->lines);
    free(layout);
}

/* ── span helpers ────────────────────────────────────────────────── */

static void line_add_span(md_display_line_t *dl, const char *text, int text_len,
                           nbs_style_t style, int display_width) {
    int n = dl->span_count;
    dl->spans = realloc(dl->spans, (size_t)(n + 1) * sizeof(md_span_t));
    ASSERT_MSG(dl->spans != NULL, "line_add_span: realloc failed");
    char *t = malloc((size_t)text_len + 1);
    memcpy(t, text, (size_t)text_len);
    t[text_len] = '\0';
    dl->spans[n].text = t;
    dl->spans[n].style = style;
    dl->spans[n].width = display_width;
    dl->span_count = n + 1;
    dl->display_width += display_width;
}

/* ── UTF-8 utilities ─────────────────────────────────────────────── */

/* Decode a UTF-8 codepoint, return bytes consumed (1-4), or 1 on error */
static int utf8_decode(const char *s, int len, uint32_t *cp) {
    if (len <= 0) { *cp = 0; return 0; }
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
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

/* Measure display width of a UTF-8 string */
static int utf8_display_width(const char *s, int len) {
    int w = 0;
    int i = 0;
    while (i < len) {
        uint32_t cp;
        int b = utf8_decode(s + i, len - i, &cp);
        if (b == 0) break;
        int cw = nbs_ts_wcwidth(cp);
        if (cw > 0) w += cw;
        i += b;
    }
    return w;
}

/* ── inline style resolution ─────────────────────────────────────── */

static const nbs_style_t *inline_style(md_inline_type_t type) {
    switch (type) {
        case MD_INLINE_BOLD:        return &MD_STYLE_BOLD;
        case MD_INLINE_ITALIC:      return &MD_STYLE_ITALIC;
        case MD_INLINE_BOLD_ITALIC: return &MD_STYLE_BOLD_ITALIC;
        case MD_INLINE_CODE:        return &MD_STYLE_INLINE_CODE;
        default:                    return &MD_STYLE_BODY;
    }
}

/* ── word fragment for reflow ────────────────────────────────────── */

typedef struct {
    char *text;
    int   byte_len;
    int   display_width;
    nbs_style_t style;
    int   is_space;    /* 1 if this is a whitespace separator */
} word_frag_t;

static void free_frags(word_frag_t *frags, int count) {
    for (int i = 0; i < count; i++) free(frags[i].text);
    free(frags);
}

/* Collect inline nodes into a flat list of word fragments for reflow */
static void collect_inline_frags(md_inline_node_t *inl, nbs_style_t parent_style,
                                  word_frag_t **frags, int *count, int *cap) {
    while (inl) {
        nbs_style_t style = parent_style;

        switch (inl->type) {
        case MD_INLINE_TEXT: {
            if (!inl->text) break;
            const char *s = inl->text;
            int len = (int)strlen(s);
            int i = 0;
            while (i < len) {
                /* skip spaces → space fragment */
                if (s[i] == ' ' || s[i] == '\t') {
                    int start = i;
                    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
                    if (*count >= *cap) {
                        *cap = (*cap == 0) ? 64 : *cap * 2;
                        *frags = realloc(*frags, (size_t)*cap * sizeof(word_frag_t));
                    }
                    word_frag_t *f = &(*frags)[*count];
                    f->byte_len = i - start;
                    f->text = malloc((size_t)f->byte_len + 1);
                    memcpy(f->text, s + start, (size_t)f->byte_len);
                    f->text[f->byte_len] = '\0';
                    f->display_width = i - start; /* spaces are width 1 each */
                    f->style = style;
                    f->is_space = 1;
                    (*count)++;
                } else {
                    /* word fragment */
                    int start = i;
                    while (i < len && s[i] != ' ' && s[i] != '\t') i++;
                    if (*count >= *cap) {
                        *cap = (*cap == 0) ? 64 : *cap * 2;
                        *frags = realloc(*frags, (size_t)*cap * sizeof(word_frag_t));
                    }
                    word_frag_t *f = &(*frags)[*count];
                    f->byte_len = i - start;
                    f->text = malloc((size_t)f->byte_len + 1);
                    memcpy(f->text, s + start, (size_t)f->byte_len);
                    f->text[f->byte_len] = '\0';
                    f->display_width = utf8_display_width(s + start, f->byte_len);
                    f->style = style;
                    f->is_space = 0;
                    (*count)++;
                }
            }
            break;
        }
        case MD_INLINE_BOLD:
        case MD_INLINE_ITALIC:
        case MD_INLINE_BOLD_ITALIC:
            style = *inline_style(inl->type);
            collect_inline_frags(inl->children, style, frags, count, cap);
            break;

        case MD_INLINE_CODE: {
            if (!inl->text) break;
            if (*count >= *cap) {
                *cap = (*cap == 0) ? 64 : *cap * 2;
                *frags = realloc(*frags, (size_t)*cap * sizeof(word_frag_t));
            }
            word_frag_t *f = &(*frags)[*count];
            f->byte_len = (int)strlen(inl->text);
            f->text = strdup(inl->text);
            f->display_width = utf8_display_width(inl->text, f->byte_len);
            f->style = MD_STYLE_INLINE_CODE;
            f->is_space = 0;
            (*count)++;
            break;
        }

        case MD_INLINE_LINK: {
            /* Link text in link style */
            if (inl->children) {
                nbs_style_t ls = MD_STYLE_LINK_TEXT;
                collect_inline_frags(inl->children, ls, frags, count, cap);
            }
            /* URL in parens, dim */
            if (inl->url) {
                /* space before URL */
                if (*count >= *cap) {
                    *cap = (*cap == 0) ? 64 : *cap * 2;
                    *frags = realloc(*frags, (size_t)*cap * sizeof(word_frag_t));
                }
                /* Build "(url)" string */
                int ulen = (int)strlen(inl->url);
                int flen = ulen + 2; /* ( + url + ) */
                char *ubuf = malloc((size_t)flen + 1);
                ubuf[0] = '(';
                memcpy(ubuf + 1, inl->url, (size_t)ulen);
                ubuf[flen - 1] = ')';
                ubuf[flen] = '\0';

                word_frag_t *f = &(*frags)[*count];
                f->byte_len = flen;
                f->text = ubuf;
                f->display_width = flen; /* ASCII URL */
                f->style = MD_STYLE_LINK_URL;
                f->is_space = 0;
                (*count)++;
            }
            break;
        }

        case MD_INLINE_SOFTBREAK:
            /* Treat as space */
            if (*count >= *cap) {
                *cap = (*cap == 0) ? 64 : *cap * 2;
                *frags = realloc(*frags, (size_t)*cap * sizeof(word_frag_t));
            }
            {
                word_frag_t *f = &(*frags)[*count];
                f->text = strdup(" ");
                f->byte_len = 1;
                f->display_width = 1;
                f->style = style;
                f->is_space = 1;
                (*count)++;
            }
            break;

        case MD_INLINE_HARDBREAK:
            /* Will be handled during reflow as forced break */
            if (*count >= *cap) {
                *cap = (*cap == 0) ? 64 : *cap * 2;
                *frags = realloc(*frags, (size_t)*cap * sizeof(word_frag_t));
            }
            {
                word_frag_t *f = &(*frags)[*count];
                f->text = strdup("\n");
                f->byte_len = 1;
                f->display_width = 0;
                f->style = style;
                f->is_space = 1;
                (*count)++;
            }
            break;
        }

        inl = inl->next;
    }
}

/* ── paragraph reflow ────────────────────────────────────────────── */

static void reflow_paragraph(md_layout_t *layout, md_block_node_t *block,
                              int terminal_width, int block_id, int indent,
                              nbs_style_t body_style) {
    word_frag_t *frags = NULL;
    int frag_count = 0;
    int frag_cap = 0;

    collect_inline_frags(block->inlines, body_style, &frags, &frag_count, &frag_cap);

    int avail = terminal_width - indent;
    if (avail < 1) avail = 1;

    md_display_line_t cur_line;
    memset(&cur_line, 0, sizeof(cur_line));
    cur_line.source_block = block_id;
    int cur_width = 0;

    /* Add indent at start of line if needed */
    if (indent > 0) {
        char *spaces = calloc(1, (size_t)indent + 1);
        memset(spaces, ' ', (size_t)indent);
        line_add_span(&cur_line, spaces, indent, body_style, indent);
        free(spaces);
    }

    int pending_space = 0; /* 1 if we need a space before the next word */
    nbs_style_t space_style = body_style;

    for (int i = 0; i < frag_count; i++) {
        word_frag_t *f = &frags[i];

        /* Hard break */
        if (f->byte_len == 1 && f->text[0] == '\n') {
            pending_space = 0;
            md_layout_add_line(layout, &cur_line);
            memset(&cur_line, 0, sizeof(cur_line));
            cur_line.source_block = block_id;
            cur_width = 0;
            if (indent > 0) {
                char *spaces = calloc(1, (size_t)indent + 1);
                memset(spaces, ' ', (size_t)indent);
                line_add_span(&cur_line, spaces, indent, body_style, indent);
                free(spaces);
            }
            continue;
        }

        /* Record spaces as pending rather than emitting immediately */
        if (f->is_space) {
            if (cur_width > 0) {
                pending_space = 1;
                space_style = f->style;
            }
            continue;
        }

        /* This is a word fragment. Check if it fits with the pending space. */
        int space_w = pending_space ? 1 : 0;
        int needed = cur_width + space_w + f->display_width;

        if (cur_width > 0 && needed > avail) {
            /* Emit current line and start new one — drop the pending space */
            pending_space = 0;
            md_layout_add_line(layout, &cur_line);
            memset(&cur_line, 0, sizeof(cur_line));
            cur_line.source_block = block_id;
            cur_width = 0;
            if (indent > 0) {
                char *spaces = calloc(1, (size_t)indent + 1);
                memset(spaces, ' ', (size_t)indent);
                line_add_span(&cur_line, spaces, indent, body_style, indent);
                free(spaces);
            }
        }

        /* Emit pending space if it fits */
        if (pending_space && cur_width > 0) {
            line_add_span(&cur_line, " ", 1, space_style, 1);
            cur_width += 1;
            pending_space = 0;
        }
        pending_space = 0;

        /* Handle words wider than available width: hard-break */
        if (f->display_width > avail) {
            /* Break character-by-character */
            const char *s = f->text;
            int slen = f->byte_len;
            int pos = 0;
            while (pos < slen) {
                int line_w = 0;
                int line_start = pos;
                while (pos < slen) {
                    uint32_t cp;
                    int b = utf8_decode(s + pos, slen - pos, &cp);
                    int cw = nbs_ts_wcwidth(cp);
                    if (cw < 0) cw = 0;
                    if (line_w + cw > avail && line_w > 0) break;
                    line_w += cw;
                    pos += b;
                }
                line_add_span(&cur_line, s + line_start, pos - line_start, f->style, line_w);
                cur_width += line_w;
                if (pos < slen) {
                    md_layout_add_line(layout, &cur_line);
                    memset(&cur_line, 0, sizeof(cur_line));
                    cur_line.source_block = block_id;
                    cur_width = 0;
                    if (indent > 0) {
                        char *spaces = calloc(1, (size_t)indent + 1);
                        memset(spaces, ' ', (size_t)indent);
                        line_add_span(&cur_line, spaces, indent, body_style, indent);
                        free(spaces);
                    }
                }
            }
            continue;
        }

        /* Normal word: add it */
        line_add_span(&cur_line, f->text, f->byte_len, f->style, f->display_width);
        cur_width += f->display_width;
    }

    /* Flush last line if it has content */
    if (cur_line.span_count > 0) {
        md_layout_add_line(layout, &cur_line);
    }

    free_frags(frags, frag_count);
}

/* ── heading rendering ───────────────────────────────────────────── */

static void render_heading(md_layout_t *layout, md_block_node_t *block,
                            int terminal_width, int block_id) {
    const nbs_style_t *style;
    int has_bg_band = 0;

    switch (block->level) {
        case 1: style = &MD_STYLE_H1; has_bg_band = 1; break;
        case 2: style = &MD_STYLE_H2; has_bg_band = 1; break;
        case 3: style = &MD_STYLE_H3; break;
        default: style = &MD_STYLE_H4; break;
    }

    /* Collect heading text */
    word_frag_t *frags = NULL;
    int frag_count = 0;
    int frag_cap = 0;
    collect_inline_frags(block->inlines, *style, &frags, &frag_count, &frag_cap);

    /* Build single line (no reflow — truncate if needed) */
    md_display_line_t hline;
    memset(&hline, 0, sizeof(hline));
    hline.source_block = block_id;

    int total_width = 0;
    for (int i = 0; i < frag_count; i++) {
        if (total_width + frags[i].display_width > terminal_width) {
            /* Truncate */
            int remain = terminal_width - total_width;
            if (remain > 0) {
                /* Add partial text */
                const char *s = frags[i].text;
                int slen = frags[i].byte_len;
                int pos = 0;
                int w = 0;
                while (pos < slen) {
                    uint32_t cp;
                    int b = utf8_decode(s + pos, slen - pos, &cp);
                    int cw = nbs_ts_wcwidth(cp);
                    if (cw < 0) cw = 0;
                    if (w + cw > remain) break;
                    w += cw;
                    pos += b;
                }
                if (pos > 0) {
                    line_add_span(&hline, s, pos, frags[i].style, w);
                    total_width += w;
                }
            }
            break;
        }
        line_add_span(&hline, frags[i].text, frags[i].byte_len, frags[i].style, frags[i].display_width);
        total_width += frags[i].display_width;
    }

    /* If H1/H2, pad with spaces for full-width background band */
    if (has_bg_band && total_width < terminal_width) {
        int pad = terminal_width - total_width;
        char *spaces = calloc(1, (size_t)pad + 1);
        memset(spaces, ' ', (size_t)pad);
        line_add_span(&hline, spaces, pad, *style, pad);
        free(spaces);
    }

    md_layout_add_line(layout, &hline);
    free_frags(frags, frag_count);
}

/* ── horizontal rule ─────────────────────────────────────────────── */

static void render_hrule(md_layout_t *layout, int terminal_width, int block_id) {
    md_display_line_t hline;
    memset(&hline, 0, sizeof(hline));
    hline.source_block = block_id;

    /* U+2500 = ─ = 0xE2 0x94 0x80, display width 1 */
    int w = terminal_width;
    int byte_len = w * 3;
    char *buf = malloc((size_t)byte_len + 1);
    for (int i = 0; i < w; i++) {
        buf[i * 3]     = (char)0xE2;
        buf[i * 3 + 1] = (char)0x94;
        buf[i * 3 + 2] = (char)0x80;
    }
    buf[byte_len] = '\0';
    line_add_span(&hline, buf, byte_len, MD_STYLE_HRULE, w);
    free(buf);

    md_layout_add_line(layout, &hline);
}

/* ── code fence ──────────────────────────────────────────────────── */

static void render_code_fence(md_layout_t *layout, md_block_node_t *block,
                               int terminal_width, int block_id) {
    /* Top border */
    {
        md_display_line_t bline;
        memset(&bline, 0, sizeof(bline));
        bline.source_block = block_id;
        bline.is_wide_line = 1;

        int lang_width = 0;
        int lang_bytes = 0;
        if (block->language) {
            lang_bytes = (int)strlen(block->language);
            lang_width = utf8_display_width(block->language, lang_bytes);
        }

        int border_chars = terminal_width - lang_width - 1; /* 1 space before lang */
        if (border_chars < 3) border_chars = 3;

        int bb = border_chars * 3;
        char *buf = malloc((size_t)bb + 1);
        for (int i = 0; i < border_chars; i++) {
            buf[i * 3]     = (char)0xE2;
            buf[i * 3 + 1] = (char)0x94;
            buf[i * 3 + 2] = (char)0x80;
        }
        buf[bb] = '\0';
        line_add_span(&bline, buf, bb, MD_STYLE_CODE_BORDER, border_chars);
        free(buf);

        if (block->language && lang_width > 0) {
            line_add_span(&bline, " ", 1, MD_STYLE_CODE_BORDER, 1);
            nbs_style_t dim_style = MD_STYLE_CODE_BORDER;
            dim_style.attrs |= NBS_ATTR_DIM;
            line_add_span(&bline, block->language, lang_bytes, dim_style, lang_width);
        }

        md_layout_add_line(layout, &bline);
    }

    /* Code content — with syntax highlighting if language is known */
    if (block->raw) {
        const char *s = block->raw;
        int slen = (int)strlen(s);
        int pos = 0;

        /* Look up language for syntax highlighting */
        const md_lang_t *lang = NULL;
        md_hl_context_t hl_ctx = MD_HL_CTX_GROUND;
        if (block->language) {
            lang = md_highlight_find_lang(block->language);
        }

        while (pos <= slen) {
            int line_start = pos;
            while (pos < slen && s[pos] != '\n') pos++;

            int ll = pos - line_start;
            md_display_line_t cline;
            memset(&cline, 0, sizeof(cline));
            cline.source_block = block_id;
            cline.is_wide_line = 1;

            if (ll > 0) {
                if (lang && lang->tokenise) {
                    /* Syntax-highlighted rendering */
                    /* Make a NUL-terminated copy of the line */
                    char *line_buf = malloc((size_t)ll + 1);
                    ASSERT_MSG(line_buf != NULL, "render_code_fence: malloc failed");
                    memcpy(line_buf, s + line_start, (size_t)ll);
                    line_buf[ll] = '\0';

                    md_hl_span_t hl_spans[128];
                    int hl_count = lang->tokenise(line_buf, &hl_ctx,
                                                   hl_spans, 128);

                    if (hl_count > 0) {
                        for (int si = 0; si < hl_count; si++) {
                            int sp_start = hl_spans[si].start;
                            int sp_len   = hl_spans[si].len;
                            if (sp_start >= ll) continue;
                            if (sp_start + sp_len > ll) sp_len = ll - sp_start;
                            if (sp_len <= 0) continue;

                            const nbs_style_t *sty =
                                md_highlight_token_style(hl_spans[si].token);
                            int dw = utf8_display_width(line_buf + sp_start,
                                                         sp_len);
                            line_add_span(&cline, line_buf + sp_start,
                                          sp_len, *sty, dw);
                        }
                    } else {
                        /* Tokeniser returned 0 spans — fallback to plain */
                        int dw = utf8_display_width(line_buf, ll);
                        line_add_span(&cline, line_buf, ll,
                                      MD_STYLE_CODE_FENCE, dw);
                    }

                    free(line_buf);
                } else {
                    /* No highlighter — plain code fence style */
                    int dw = utf8_display_width(s + line_start, ll);
                    line_add_span(&cline, s + line_start, ll,
                                  MD_STYLE_CODE_FENCE, dw);
                }
            }

            md_layout_add_line(layout, &cline);
            pos++; /* skip \n */
            if (pos > slen) break; /* don't emit trailing empty line for \n at end */
        }
    }

    /* Bottom border */
    {
        md_display_line_t bline;
        memset(&bline, 0, sizeof(bline));
        bline.source_block = block_id;
        bline.is_wide_line = 1;

        int bb = terminal_width * 3;
        char *buf = malloc((size_t)bb + 1);
        for (int i = 0; i < terminal_width; i++) {
            buf[i * 3]     = (char)0xE2;
            buf[i * 3 + 1] = (char)0x94;
            buf[i * 3 + 2] = (char)0x80;
        }
        buf[bb] = '\0';
        line_add_span(&bline, buf, bb, MD_STYLE_CODE_BORDER, terminal_width);
        free(buf);

        md_layout_add_line(layout, &bline);
    }
}

/* ── list rendering ──────────────────────────────────────────────── */

/* U+2022 = • (bullet), U+25E6 = ◦, U+25AA = ▪ */
static const char *bullet_chars[] = {
    "\xe2\x80\xa2",  /* • */
    "\xe2\x97\xa6",  /* ◦ */
    "\xe2\x96\xaa"   /* ▪ */
};

static void render_list(md_layout_t *layout, md_block_node_t *list,
                         int terminal_width, int block_id, int depth) {
    int item_num = list->start > 0 ? list->start : 1;
    int indent = depth * 3;

    md_block_node_t *item = list->children;
    while (item) {
        if (item->type == MD_BLOCK_LIST_ITEM) {
            /* Render marker */
            char marker_buf[32];
            int marker_len;
            int marker_width;

            if (list->ordered) {
                if (depth == 0) {
                    marker_len = snprintf(marker_buf, sizeof(marker_buf), "%d. ", item_num);
                } else if (depth == 1) {
                    marker_buf[0] = (char)('a' + (item_num - 1) % 26);
                    marker_buf[1] = '.';
                    marker_buf[2] = ' ';
                    marker_buf[3] = '\0';
                    marker_len = 3;
                } else {
                    /* Roman numerals (simplified) */
                    const char *roman[] = {"i","ii","iii","iv","v","vi","vii","viii","ix","x"};
                    int ri = (item_num - 1) % 10;
                    marker_len = snprintf(marker_buf, sizeof(marker_buf), "%s. ", roman[ri]);
                }
                marker_width = marker_len;
                item_num++;
            } else {
                int bi = depth % 3;
                int blen = 3; /* all bullets are 3 bytes UTF-8 */
                memcpy(marker_buf, bullet_chars[bi], (size_t)blen);
                marker_buf[blen] = ' ';
                marker_buf[blen + 1] = '\0';
                marker_len = blen + 1;
                marker_width = 2; /* bullet (1 col) + space (1 col) */
            }

            /* Render item content with indentation */
            int total_indent = indent + marker_width;

            /* First, emit the marker */
            md_block_node_t *child = item->children;
            if (child && child->type == MD_BLOCK_PARAGRAPH) {
                /* Create a temporary paragraph with the marker prepended */
                md_display_line_t first_line;
                memset(&first_line, 0, sizeof(first_line));
                first_line.source_block = block_id;

                /* Add indent spaces */
                if (indent > 0) {
                    char *spaces = calloc(1, (size_t)indent + 1);
                    memset(spaces, ' ', (size_t)indent);
                    line_add_span(&first_line, spaces, indent, MD_STYLE_BODY, indent);
                    free(spaces);
                }

                /* Add marker */
                line_add_span(&first_line, marker_buf, marker_len, MD_STYLE_LIST_MARKER, marker_width);

                /* Now reflow the paragraph content with total_indent for continuation lines */
                word_frag_t *frags = NULL;
                int frag_count = 0;
                int frag_cap = 0;
                collect_inline_frags(child->inlines, MD_STYLE_BODY, &frags, &frag_count, &frag_cap);

                int avail = terminal_width - total_indent;
                if (avail < 1) avail = 1;
                int cur_width = 0;
                int list_pending_space = 0;

                for (int i = 0; i < frag_count; i++) {
                    word_frag_t *f = &frags[i];
                    if (f->is_space) {
                        if (cur_width > 0) list_pending_space = 1;
                        continue;
                    }

                    int sw = list_pending_space ? 1 : 0;
                    if (cur_width > 0 && cur_width + sw + f->display_width > avail) {
                        /* Emit current line, start continuation */
                        list_pending_space = 0;
                        md_layout_add_line(layout, &first_line);
                        memset(&first_line, 0, sizeof(first_line));
                        first_line.source_block = block_id;
                        cur_width = 0;
                        /* Continuation indent */
                        if (total_indent > 0) {
                            char *spaces = calloc(1, (size_t)total_indent + 1);
                            memset(spaces, ' ', (size_t)total_indent);
                            line_add_span(&first_line, spaces, total_indent, MD_STYLE_BODY, total_indent);
                            free(spaces);
                        }
                    }

                    if (list_pending_space && cur_width > 0) {
                        line_add_span(&first_line, " ", 1, MD_STYLE_BODY, 1);
                        cur_width += 1;
                        list_pending_space = 0;
                    }
                    list_pending_space = 0;

                    line_add_span(&first_line, f->text, f->byte_len, f->style, f->display_width);
                    cur_width += f->display_width;
                }

                if (first_line.span_count > 0) {
                    md_layout_add_line(layout, &first_line);
                }

                free_frags(frags, frag_count);

                /* Render remaining children of item */
                child = child->next;
                while (child) {
                    if (child->type == MD_BLOCK_LIST) {
                        render_list(layout, child, terminal_width, block_id, depth + 1);
                    } else if (child->type == MD_BLOCK_PARAGRAPH) {
                        reflow_paragraph(layout, child, terminal_width, block_id,
                                        total_indent, MD_STYLE_BODY);
                    }
                    child = child->next;
                }
            }
        }
        item = item->next;
    }
}

/* ── blockquote rendering ────────────────────────────────────────── */

static void render_blockquote(md_layout_t *layout, md_block_node_t *bq,
                               int terminal_width, int block_id) {
    /* Render children, then prepend "│ " bar to each line */
    md_layout_t *inner = md_render(bq, terminal_width - 4);

    /* U+2502 = │ = 0xE2 0x94 0x82 */
    for (int i = 0; i < inner->line_count; i++) {
        md_display_line_t *src = &inner->lines[i];
        md_display_line_t dest;
        memset(&dest, 0, sizeof(dest));
        dest.source_block = block_id;

        /* Add bar */
        line_add_span(&dest, "\xe2\x94\x82 ", 4, MD_STYLE_BLOCKQUOTE_BAR, 2);

        /* Copy spans, changing style to blockquote text */
        for (int j = 0; j < src->span_count; j++) {
            line_add_span(&dest, src->spans[j].text, (int)strlen(src->spans[j].text),
                         MD_STYLE_BLOCKQUOTE_TEXT, src->spans[j].width);
        }

        md_layout_add_line(layout, &dest);
    }

    /* Don't free inner spans since we copied them — but do free the layout structure */
    for (int i = 0; i < inner->line_count; i++) {
        for (int j = 0; j < inner->lines[i].span_count; j++) {
            free(inner->lines[i].spans[j].text);
        }
        free(inner->lines[i].spans);
    }
    free(inner->lines);
    free(inner);
}

/* ── main render function ────────────────────────────────────────── */

/* Check if last line in layout is blank */
static int last_line_is_blank(md_layout_t *layout) {
    if (layout->line_count == 0) return 1; /* treat start as "blank" */
    md_display_line_t *last = &layout->lines[layout->line_count - 1];
    return last->span_count == 0;
}

/* Add blank line if the last line isn't already blank (no double-blanking) */
static void ensure_blank_before(md_layout_t *layout, int block_id) {
    if (!last_line_is_blank(layout)) {
        md_layout_add_blank(layout, block_id);
    }
}

md_layout_t *md_render(md_block_node_t *root, int terminal_width) {
    md_layout_t *layout = calloc(1, sizeof(*layout));
    ASSERT_MSG(layout != NULL, "md_render: failed to allocate layout");

    if (!root) return layout;
    if (terminal_width < 1) terminal_width = 1;

    md_block_node_t *child = (root->type == MD_BLOCK_DOCUMENT) ? root->children : root;
    /* If root is not DOCUMENT, treat as a single-child scenario (for blockquote recursion) */
    if (root->type != MD_BLOCK_DOCUMENT && root->type != MD_BLOCK_BLOCKQUOTE) {
        child = root;
    } else {
        child = root->children;
    }

    int block_id = 0;

    while (child) {
        switch (child->type) {
        case MD_BLOCK_PARAGRAPH:
            if (layout->line_count > 0)
                ensure_blank_before(layout, block_id);
            reflow_paragraph(layout, child, terminal_width, block_id, 0, MD_STYLE_BODY);
            break;

        case MD_BLOCK_HEADING:
            ensure_blank_before(layout, block_id);
            render_heading(layout, child, terminal_width, block_id);
            /* H1/H2 get trailing blank, H3/H4 do not */
            if (child->level <= 2) {
                md_layout_add_blank(layout, block_id);
            }
            break;

        case MD_BLOCK_HRULE:
            ensure_blank_before(layout, block_id);
            render_hrule(layout, terminal_width, block_id);
            md_layout_add_blank(layout, block_id);
            break;

        case MD_BLOCK_CODE_FENCE:
            ensure_blank_before(layout, block_id);
            render_code_fence(layout, child, terminal_width, block_id);
            md_layout_add_blank(layout, block_id);
            break;

        case MD_BLOCK_LIST:
            ensure_blank_before(layout, block_id);
            render_list(layout, child, terminal_width, block_id, 0);
            break;

        case MD_BLOCK_TABLE:
            ensure_blank_before(layout, block_id);
            md_table_render(layout, child, terminal_width, block_id);
            md_layout_add_blank(layout, block_id);
            break;

        case MD_BLOCK_BLOCKQUOTE:
            ensure_blank_before(layout, block_id);
            render_blockquote(layout, child, terminal_width, block_id);
            md_layout_add_blank(layout, block_id);
            break;

        default:
            break;
        }

        block_id++;
        child = child->next;
    }

    return layout;
}
