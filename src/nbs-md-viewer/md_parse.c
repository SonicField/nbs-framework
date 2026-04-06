/*
 * md_parse.c — Markdown parser.
 *
 * Two-pass: block structure (line-by-line classification), then inline parsing.
 * Supports: headings (#-####), paragraphs, horizontal rules, code fences,
 * ordered/unordered lists with nesting, blockquotes, tables.
 * Inline: **bold**, *italic*, ***bold italic***, `code`, [text](url),
 *         soft breaks, hard breaks.
 */

#include "md_parse.h"
#include "../nbs-common/nbs_assert.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── helpers ─────────────────────────────────────────────────────── */

static int is_blank_line(const char *line, int len) {
    for (int i = 0; i < len; i++) {
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
            return 0;
    }
    return 1;
}

/* Count leading spaces */
static int count_leading_spaces(const char *line, int len) {
    int n = 0;
    for (int i = 0; i < len && line[i] == ' '; i++) n++;
    return n;
}

/* Check if line is a horizontal rule: 3+ of same char (-, *, _) optionally with spaces */
static int is_hrule(const char *line, int len) {
    char ch = 0;
    int count = 0;
    for (int i = 0; i < len; i++) {
        if (line[i] == ' ' || line[i] == '\t') continue;
        if (line[i] == '\r') continue;
        if (ch == 0) {
            if (line[i] == '-' || line[i] == '*' || line[i] == '_')
                ch = line[i];
            else
                return 0;
        }
        if (line[i] != ch) return 0;
        count++;
    }
    return count >= 3;
}

/* Check heading: returns level 1-4, or 0 if not a heading */
static int heading_level(const char *line, int len) {
    int level = 0;
    int i = 0;
    while (i < len && line[i] == '#') { level++; i++; }
    if (level < 1 || level > 4) return 0;
    if (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\0')
        return 0;
    return level;
}

/* Check if line starts a code fence: ``` optionally followed by language tag */
static int is_code_fence_start(const char *line, int len, const char **lang_start, int *lang_len) {
    int i = 0;
    /* skip up to 3 leading spaces */
    while (i < len && i < 3 && line[i] == ' ') i++;
    int tick_count = 0;
    while (i < len && line[i] == '`') { tick_count++; i++; }
    if (tick_count < 3) return 0;
    /* skip spaces before language */
    while (i < len && line[i] == ' ') i++;
    *lang_start = line + i;
    int ls = 0;
    while (i < len && line[i] != ' ' && line[i] != '\r' && line[i] != '\n') {
        ls++; i++;
    }
    *lang_len = ls;
    return 1;
}

static int is_code_fence_end(const char *line, int len) {
    int i = 0;
    while (i < len && i < 3 && line[i] == ' ') i++;
    int tick_count = 0;
    while (i < len && line[i] == '`') { tick_count++; i++; }
    if (tick_count < 3) return 0;
    /* rest must be blank */
    while (i < len) {
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') return 0;
        i++;
    }
    return 1;
}

/* Check unordered list item: -, *, + followed by space */
static int is_unordered_list_item(const char *line, int len, int offset) {
    if (offset >= len) return 0;
    char c = line[offset];
    if ((c == '-' || c == '*' || c == '+') && offset + 1 < len && line[offset + 1] == ' ')
        return 1;
    return 0;
}

/* Check ordered list item: digits followed by . or ) and space */
static int is_ordered_list_item(const char *line, int len, int offset, int *start_num) {
    int i = offset;
    if (i >= len || !isdigit((unsigned char)line[i])) return 0;
    int num = 0;
    while (i < len && isdigit((unsigned char)line[i])) {
        num = num * 10 + (line[i] - '0');
        i++;
    }
    if (i >= len) return 0;
    if (line[i] != '.' && line[i] != ')') return 0;
    i++;
    if (i >= len || line[i] != ' ') return 0;
    *start_num = num;
    return 1;
}

/* Check if line starts a table row */
static int is_table_row(const char *line, int len) {
    int i = 0;
    while (i < len && line[i] == ' ') i++;
    if (i < len && line[i] == '|') return 1;
    return 0;
}

/* Check if line is a table separator row: |:---|:---:|---:| */
static int is_table_separator(const char *line, int len) {
    int i = 0;
    while (i < len && line[i] == ' ') i++;
    if (i >= len || line[i] != '|') return 0;
    i++;
    int has_dash = 0;
    while (i < len) {
        if (line[i] == '-' || line[i] == ':' || line[i] == '|' || line[i] == ' ')
            { if (line[i] == '-') has_dash = 1; i++; }
        else if (line[i] == '\r') { i++; }
        else return 0;
    }
    return has_dash;
}

/* Check blockquote: > at start (after optional spaces) */
static int is_blockquote(const char *line, int len, int *content_offset) {
    int i = 0;
    while (i < len && i < 3 && line[i] == ' ') i++;
    if (i < len && line[i] == '>') {
        i++;
        if (i < len && line[i] == ' ') i++;
        *content_offset = i;
        return 1;
    }
    return 0;
}

/* ── strndup for lines ───────────────────────────────────────────── */

static char *line_strdup(const char *s, int len) {
    /* trim trailing \r */
    while (len > 0 && s[len - 1] == '\r') len--;
    char *r = malloc((size_t)len + 1);
    if (!r) return NULL;
    memcpy(r, s, (size_t)len);
    r[len] = '\0';
    return r;
}

/* ── line iterator ───────────────────────────────────────────────── */

typedef struct {
    const char *data;
    int pos;
    int len;
} line_iter_t;

static void line_iter_init(line_iter_t *it, const char *data) {
    it->data = data ? data : "";
    it->pos = 0;
    it->len = (int)strlen(it->data);
}

/* Returns pointer to start of next line, sets *line_len. Returns NULL at end. */
static const char *line_iter_next(line_iter_t *it, int *line_len) {
    if (it->pos >= it->len) return NULL;
    const char *start = it->data + it->pos;
    int i = it->pos;
    while (i < it->len && it->data[i] != '\n') i++;
    *line_len = i - it->pos;
    if (i < it->len) i++; /* skip \n */
    it->pos = i;
    return start;
}

/* ── inline parser ───────────────────────────────────────────────── */

static void parse_inlines(md_block_node_t *block, const char *text, int len);

/* Parse inline content and append inline nodes to the block.
 * Handles: ***bold italic***, **bold**, *italic*, `code`, [text](url) */
static void parse_inline_content(md_block_node_t *block, const char *text, int len) {
    int i = 0;
    int text_start = 0;

    while (i < len) {
        /* Hard break: \n sentinel inserted by PARA_APPEND for 2+ trailing spaces */
        if (text[i] == '\n') {
            /* flush preceding text */
            if (i > text_start) {
                md_inline_node_t *tn = md_inline_create(MD_INLINE_TEXT);
                tn->text = line_strdup(text + text_start, i - text_start);
                md_block_add_inline(block, tn);
            }
            md_inline_node_t *hb = md_inline_create(MD_INLINE_HARDBREAK);
            md_block_add_inline(block, hb);
            i++;
            text_start = i;
            continue;
        }

        /* backtick: inline code */
        if (text[i] == '`') {
            /* flush preceding text */
            if (i > text_start) {
                md_inline_node_t *tn = md_inline_create(MD_INLINE_TEXT);
                tn->text = line_strdup(text + text_start, i - text_start);
                md_block_add_inline(block, tn);
            }
            int j = i + 1;
            while (j < len && text[j] != '`') j++;
            if (j < len) {
                md_inline_node_t *cn = md_inline_create(MD_INLINE_CODE);
                cn->text = line_strdup(text + i + 1, j - i - 1);
                md_block_add_inline(block, cn);
                i = j + 1;
                text_start = i;
            } else {
                /* unclosed backtick — treat as text */
                i++;
            }
            continue;
        }

        /* link: [text](url) */
        if (text[i] == '[') {
            int bracket_end = -1;
            int depth = 1;
            int j = i + 1;
            while (j < len && depth > 0) {
                if (text[j] == '[') depth++;
                else if (text[j] == ']') { depth--; if (depth == 0) bracket_end = j; }
                j++;
            }
            if (bracket_end > 0 && bracket_end + 1 < len && text[bracket_end + 1] == '(') {
                int paren_end = -1;
                j = bracket_end + 2;
                while (j < len) {
                    if (text[j] == ')') { paren_end = j; break; }
                    j++;
                }
                if (paren_end > 0) {
                    /* flush preceding text */
                    if (i > text_start) {
                        md_inline_node_t *tn = md_inline_create(MD_INLINE_TEXT);
                        tn->text = line_strdup(text + text_start, i - text_start);
                        md_block_add_inline(block, tn);
                    }
                    md_inline_node_t *ln = md_inline_create(MD_INLINE_LINK);
                    ln->url = line_strdup(text + bracket_end + 2, paren_end - bracket_end - 2);
                    /* parse link text as inline children */
                    int link_text_len = bracket_end - i - 1;
                    if (link_text_len > 0) {
                        md_inline_node_t *lt = md_inline_create(MD_INLINE_TEXT);
                        lt->text = line_strdup(text + i + 1, link_text_len);
                        md_inline_add_child(ln, lt);
                    }
                    md_block_add_inline(block, ln);
                    i = paren_end + 1;
                    text_start = i;
                    continue;
                }
            }
            /* not a valid link, continue as text */
            i++;
            continue;
        }

        /* bold italic, bold, or italic with * or _ */
        if (text[i] == '*' || text[i] == '_') {
            char delim = text[i];

            /* Underscore word-boundary rule (CommonMark §6.2):
             * _ cannot open emphasis if preceded by alphanumeric.
             * * has no such restriction — intra-word *foo* is valid. */
            if (delim == '_' && i > 0 && isalnum((unsigned char)text[i - 1])) {
                i++;
                continue;
            }

            int run = 0;
            int k = i;
            while (k < len && text[k] == delim) { run++; k++; }

            if (run >= 3) {
                /* find closing *** or ___ */
                int close = -1;
                for (int s = k; s + 2 < len; s++) {
                    if (text[s] == delim && text[s+1] == delim && text[s+2] == delim) {
                        /* _ closer must not be followed by alnum */
                        if (delim == '_' && s + 3 < len && isalnum((unsigned char)text[s + 3])) continue;
                        close = s;
                        break;
                    }
                }
                if (close >= 0) {
                    if (i > text_start) {
                        md_inline_node_t *tn = md_inline_create(MD_INLINE_TEXT);
                        tn->text = line_strdup(text + text_start, i - text_start);
                        md_block_add_inline(block, tn);
                    }
                    md_inline_node_t *bi = md_inline_create(MD_INLINE_BOLD_ITALIC);
                    md_inline_node_t *ct = md_inline_create(MD_INLINE_TEXT);
                    ct->text = line_strdup(text + i + 3, close - i - 3);
                    md_inline_add_child(bi, ct);
                    md_block_add_inline(block, bi);
                    i = close + 3;
                    text_start = i;
                    continue;
                }
            }

            if (run >= 2) {
                /* find closing ** or __ */
                int close = -1;
                for (int s = i + 2; s + 1 < len; s++) {
                    if (text[s] == delim && text[s+1] == delim) {
                        /* make sure it's not *** */
                        if (run == 2 || (s + 2 >= len || text[s+2] != delim)) {
                            /* _ closer must not be followed by alnum */
                            if (delim == '_' && s + 2 < len && isalnum((unsigned char)text[s + 2])) continue;
                            close = s;
                            break;
                        }
                    }
                }
                if (close >= 0) {
                    if (i > text_start) {
                        md_inline_node_t *tn = md_inline_create(MD_INLINE_TEXT);
                        tn->text = line_strdup(text + text_start, i - text_start);
                        md_block_add_inline(block, tn);
                    }
                    md_inline_node_t *bn = md_inline_create(MD_INLINE_BOLD);
                    md_inline_node_t *ct = md_inline_create(MD_INLINE_TEXT);
                    ct->text = line_strdup(text + i + 2, close - i - 2);
                    md_inline_add_child(bn, ct);
                    md_block_add_inline(block, bn);
                    i = close + 2;
                    text_start = i;
                    continue;
                }
            }

            if (run >= 1) {
                /* find closing * or _ */
                int close = -1;
                for (int s = i + 1; s < len; s++) {
                    if (text[s] == delim) {
                        /* don't match ** as italic close */
                        if (run == 1 || (s + 1 >= len || text[s+1] != delim)) {
                            /* _ closer must not be followed by alnum */
                            if (delim == '_' && s + 1 < len && isalnum((unsigned char)text[s + 1])) continue;
                            close = s;
                            break;
                        }
                    }
                }
                if (close >= 0) {
                    if (i > text_start) {
                        md_inline_node_t *tn = md_inline_create(MD_INLINE_TEXT);
                        tn->text = line_strdup(text + text_start, i - text_start);
                        md_block_add_inline(block, tn);
                    }
                    md_inline_node_t *in_node = md_inline_create(MD_INLINE_ITALIC);
                    md_inline_node_t *ct = md_inline_create(MD_INLINE_TEXT);
                    ct->text = line_strdup(text + i + 1, close - i - 1);
                    md_inline_add_child(in_node, ct);
                    md_block_add_inline(block, in_node);
                    i = close + 1;
                    text_start = i;
                    continue;
                }
            }

            /* no closing found, treat as text */
            i++;
            continue;
        }

        i++;
    }

    /* flush remaining text */
    if (i > text_start) {
        md_inline_node_t *tn = md_inline_create(MD_INLINE_TEXT);
        tn->text = line_strdup(text + text_start, i - text_start);
        md_block_add_inline(block, tn);
    }
}

static void parse_inlines(md_block_node_t *block, const char *text, int len) {
    parse_inline_content(block, text, len);
}

/* ── table parser ────────────────────────────────────────────────── */

/* Parse a single table row into cells, return cell count */
static int parse_table_cells(const char *line, int len, char **cells, int max_cells) {
    int i = 0;
    int cell_count = 0;

    /* skip leading whitespace */
    while (i < len && line[i] == ' ') i++;
    /* skip leading | */
    if (i < len && line[i] == '|') i++;

    while (i < len && cell_count < max_cells) {
        /* skip trailing \r */
        if (line[i] == '\r' || line[i] == '\n') break;

        int cell_start = i;
        while (i < len && line[i] != '|' && line[i] != '\r' && line[i] != '\n') i++;

        /* trim spaces from cell content */
        int cs = cell_start, ce = i;
        while (cs < ce && line[cs] == ' ') cs++;
        while (ce > cs && line[ce - 1] == ' ') ce--;

        cells[cell_count] = line_strdup(line + cs, ce - cs);
        cell_count++;

        if (i < len && line[i] == '|') i++;
    }

    /* if last cell is empty (trailing |), remove it */
    if (cell_count > 0 && cells[cell_count - 1][0] == '\0') {
        free(cells[cell_count - 1]);
        cell_count--;
    }

    return cell_count;
}

/* Parse alignment from separator row */
static void parse_table_alignments(const char *line, int len, md_align_t *aligns, int max_cols) {
    int i = 0;
    int col = 0;

    while (i < len && line[i] == ' ') i++;
    if (i < len && line[i] == '|') i++;

    while (i < len && col < max_cols) {
        if (line[i] == '\r' || line[i] == '\n') break;

        int start = i;
        while (i < len && line[i] != '|' && line[i] != '\r' && line[i] != '\n') i++;
        int end = i;

        /* trim spaces */
        while (start < end && line[start] == ' ') start++;
        while (end > start && line[end - 1] == ' ') end--;

        int left_colon = (start < end && line[start] == ':');
        int right_colon = (end > start && line[end - 1] == ':');

        if (left_colon && right_colon) aligns[col] = MD_ALIGN_CENTRE;
        else if (right_colon) aligns[col] = MD_ALIGN_RIGHT;
        else aligns[col] = MD_ALIGN_LEFT;

        col++;
        if (i < len && line[i] == '|') i++;
    }
}

/* ── block parser (pass 1) ───────────────────────────────────────── */

md_block_node_t *md_parse(const char *input) {
    md_block_node_t *doc = md_block_create(MD_BLOCK_DOCUMENT);
    ASSERT_MSG(doc != NULL, "md_parse: failed to allocate document node");

    if (!input || !*input) return doc;

    line_iter_t iter;
    line_iter_init(&iter, input);

    const char *line;
    int line_len;

    /* Paragraph accumulator */
    char *para_buf = NULL;
    int para_len = 0;
    int para_cap = 0;

    /* Helper to flush accumulated paragraph text */
    #define FLUSH_PARA() do { \
        if (para_len > 0) { \
            md_block_node_t *p = md_block_create(MD_BLOCK_PARAGRAPH); \
            parse_inlines(p, para_buf, para_len); \
            md_block_add_child(doc, p); \
            para_len = 0; \
        } \
    } while(0)

    #define PARA_APPEND(s, l) do { \
        int _needed = para_len + (l) + 2; \
        if (_needed > para_cap) { \
            para_cap = _needed * 2; \
            para_buf = realloc(para_buf, (size_t)para_cap); \
        } \
        if (para_len > 0) { \
            /* Check if previous line ended with 2+ spaces (hard break). \
             * If so, replace trailing spaces with \n sentinel. */ \
            int _trailing = 0; \
            while (_trailing < para_len && para_buf[para_len - 1 - _trailing] == ' ') \
                _trailing++; \
            if (_trailing >= 2) { \
                para_len -= _trailing; \
                para_buf[para_len++] = '\n'; \
            } else { \
                para_buf[para_len++] = ' '; \
            } \
        } \
        memcpy(para_buf + para_len, (s), (size_t)(l)); \
        para_len += (l); \
        para_buf[para_len] = '\0'; \
    } while(0)

    while ((line = line_iter_next(&iter, &line_len)) != NULL) {
        /* Trim trailing \r for all checks */
        int tlen = line_len;
        while (tlen > 0 && line[tlen - 1] == '\r') tlen--;

        /* Blank line */
        if (is_blank_line(line, tlen)) {
            FLUSH_PARA();
            continue;
        }

        /* Code fence */
        const char *lang_start;
        int lang_len;
        if (is_code_fence_start(line, tlen, &lang_start, &lang_len)) {
            FLUSH_PARA();
            md_block_node_t *cf = md_block_create(MD_BLOCK_CODE_FENCE);
            if (lang_len > 0) {
                cf->language = line_strdup(lang_start, lang_len);
            }
            /* collect body until closing fence or EOF */
            char *body = NULL;
            int body_len = 0;
            int body_cap = 0;
            const char *cline;
            int clen;
            while ((cline = line_iter_next(&iter, &clen)) != NULL) {
                int ctlen = clen;
                while (ctlen > 0 && cline[ctlen - 1] == '\r') ctlen--;
                if (is_code_fence_end(cline, ctlen)) break;
                int needed = body_len + ctlen + 2;
                if (needed > body_cap) {
                    body_cap = needed * 2;
                    body = realloc(body, (size_t)body_cap);
                }
                if (body_len > 0) body[body_len++] = '\n';
                memcpy(body + body_len, cline, (size_t)ctlen);
                body_len += ctlen;
            }
            if (body) {
                body[body_len] = '\0';
                cf->raw = body;
            } else {
                cf->raw = strdup("");
            }
            md_block_add_child(doc, cf);
            continue;
        }

        /* Heading */
        int hlevel = heading_level(line, tlen);
        if (hlevel > 0) {
            FLUSH_PARA();
            md_block_node_t *h = md_block_create(MD_BLOCK_HEADING);
            h->level = hlevel;
            /* skip # chars and space */
            int skip = hlevel;
            while (skip < tlen && (line[skip] == ' ' || line[skip] == '\t')) skip++;
            /* trim trailing # and spaces */
            int hend = tlen;
            while (hend > skip && line[hend - 1] == '#') hend--;
            while (hend > skip && line[hend - 1] == ' ') hend--;
            if (hend > skip) {
                parse_inlines(h, line + skip, hend - skip);
            }
            md_block_add_child(doc, h);
            continue;
        }

        /* Horizontal rule */
        if (is_hrule(line, tlen)) {
            FLUSH_PARA();
            md_block_node_t *hr = md_block_create(MD_BLOCK_HRULE);
            md_block_add_child(doc, hr);
            continue;
        }

        /* Table: detect by looking ahead for separator row */
        if (is_table_row(line, tlen)) {
            /* peek at next line */
            int saved_pos = iter.pos;
            const char *next_line;
            int next_len;
            next_line = line_iter_next(&iter, &next_len);
            int next_tlen = next_len;
            if (next_line) {
                while (next_tlen > 0 && next_line[next_tlen - 1] == '\r') next_tlen--;
            }

            if (next_line && is_table_separator(next_line, next_tlen)) {
                /* It's a table */
                FLUSH_PARA();

                #define MAX_TABLE_COLS 64
                char *header_cells[MAX_TABLE_COLS];
                int col_count = parse_table_cells(line, tlen, header_cells, MAX_TABLE_COLS);

                md_block_node_t *table = md_block_create(MD_BLOCK_TABLE);
                table->col_count = col_count;
                table->col_align = calloc((size_t)col_count, sizeof(md_align_t));
                parse_table_alignments(next_line, next_tlen, table->col_align, col_count);

                /* header row */
                md_block_node_t *hrow = md_block_create(MD_BLOCK_TABLE_ROW);
                hrow->is_header = 1;
                for (int c = 0; c < col_count; c++) {
                    md_block_node_t *cell = md_block_create(MD_BLOCK_TABLE_CELL);
                    int clen2 = (int)strlen(header_cells[c]);
                    parse_inlines(cell, header_cells[c], clen2);
                    free(header_cells[c]);
                    md_block_add_child(hrow, cell);
                }
                md_block_add_child(table, hrow);

                /* body rows */
                const char *tline;
                int tlen2;
                while ((tline = line_iter_next(&iter, &tlen2)) != NULL) {
                    int ttlen = tlen2;
                    while (ttlen > 0 && tline[ttlen - 1] == '\r') ttlen--;
                    if (!is_table_row(tline, ttlen) || is_blank_line(tline, ttlen)) break;

                    char *body_cells[MAX_TABLE_COLS];
                    int bc = parse_table_cells(tline, ttlen, body_cells, MAX_TABLE_COLS);

                    md_block_node_t *brow = md_block_create(MD_BLOCK_TABLE_ROW);
                    brow->is_header = 0;
                    for (int c = 0; c < col_count; c++) {
                        md_block_node_t *cell = md_block_create(MD_BLOCK_TABLE_CELL);
                        if (c < bc) {
                            int clen3 = (int)strlen(body_cells[c]);
                            parse_inlines(cell, body_cells[c], clen3);
                        }
                        md_block_add_child(brow, cell);
                    }
                    for (int c = 0; c < bc; c++) free(body_cells[c]);
                    md_block_add_child(table, brow);
                }

                md_block_add_child(doc, table);
                continue;
            } else {
                /* Not a table, restore position and fall through to paragraph */
                iter.pos = saved_pos;
            }
        }

        /* Blockquote */
        int bq_offset;
        if (is_blockquote(line, tlen, &bq_offset)) {
            FLUSH_PARA();
            md_block_node_t *bq = md_block_create(MD_BLOCK_BLOCKQUOTE);

            /* Accumulate blockquote content */
            char *bq_buf = NULL;
            int bq_len = 0;
            int bq_cap = 0;

            /* First line */
            int content_len = tlen - bq_offset;
            if (content_len > 0) {
                bq_cap = content_len + 2;
                bq_buf = malloc((size_t)bq_cap);
                memcpy(bq_buf, line + bq_offset, (size_t)content_len);
                bq_len = content_len;
                bq_buf[bq_len] = '\0';
            }

            /* Continue reading blockquote lines */
            while (1) {
                int saved = iter.pos;
                const char *bline = line_iter_next(&iter, &line_len);
                if (!bline) break;
                int btlen = line_len;
                while (btlen > 0 && bline[btlen - 1] == '\r') btlen--;

                int bq_off2;
                if (is_blockquote(bline, btlen, &bq_off2)) {
                    int cl = btlen - bq_off2;
                    int needed = bq_len + cl + 2;
                    if (needed > bq_cap) { bq_cap = needed * 2; bq_buf = realloc(bq_buf, (size_t)bq_cap); }
                    if (bq_len > 0) bq_buf[bq_len++] = '\n';
                    if (cl > 0) memcpy(bq_buf + bq_len, bline + bq_off2, (size_t)cl);
                    bq_len += cl;
                    if (bq_buf) bq_buf[bq_len] = '\0';
                } else {
                    iter.pos = saved;
                    break;
                }
            }

            /* Parse blockquote content recursively */
            if (bq_buf && bq_len > 0) {
                md_block_node_t *inner = md_parse(bq_buf);
                /* steal inner's children */
                md_block_node_t *child = inner->children;
                inner->children = NULL;
                while (child) {
                    md_block_node_t *next = child->next;
                    child->next = NULL;
                    md_block_add_child(bq, child);
                    child = next;
                }
                md_block_destroy(inner);
            }
            free(bq_buf);

            md_block_add_child(doc, bq);
            continue;
        }

        /* Unordered list */
        int leading = count_leading_spaces(line, tlen);
        if (is_unordered_list_item(line, tlen, leading)) {
            FLUSH_PARA();
            int base_indent = leading;

            md_block_node_t *list = md_block_create(MD_BLOCK_LIST);
            list->ordered = 0;

            /* Parse first item */
            int content_start = leading + 2;
            md_block_node_t *item = md_block_create(MD_BLOCK_LIST_ITEM);
            item->level = 0;
            if (content_start < tlen) {
                md_block_node_t *p = md_block_create(MD_BLOCK_PARAGRAPH);
                parse_inlines(p, line + content_start, tlen - content_start);
                md_block_add_child(item, p);
            }
            md_block_add_child(list, item);

            md_block_node_t *last_item = item;

            /* Continue reading list items */
            while (1) {
                int saved = iter.pos;
                const char *lline = line_iter_next(&iter, &line_len);
                if (!lline) break;
                int ltlen = line_len;
                while (ltlen > 0 && lline[ltlen - 1] == '\r') ltlen--;

                if (is_blank_line(lline, ltlen)) break;

                int ll = count_leading_spaces(lline, ltlen);
                int sn_dummy;
                if (is_unordered_list_item(lline, ltlen, ll) || is_ordered_list_item(lline, ltlen, ll, &sn_dummy)) {
                    if (ll > base_indent) {
                        /* Nested sublist — add as child of last_item */
                        md_block_node_t *sublist = md_block_create(MD_BLOCK_LIST);
                        int sub_ordered = is_ordered_list_item(lline, ltlen, ll, &sn_dummy);
                        sublist->ordered = sub_ordered;
                        if (sub_ordered) sublist->start = sn_dummy;

                        /* First subitem */
                        int sub_cs;
                        if (sub_ordered) {
                            sub_cs = ll;
                            while (sub_cs < ltlen && (lline[sub_cs] >= '0' && lline[sub_cs] <= '9')) sub_cs++;
                            sub_cs++; /* skip . or ) */
                            if (sub_cs < ltlen && lline[sub_cs] == ' ') sub_cs++;
                        } else {
                            sub_cs = ll + 2;
                        }
                        md_block_node_t *si = md_block_create(MD_BLOCK_LIST_ITEM);
                        if (sub_cs < ltlen) {
                            md_block_node_t *p = md_block_create(MD_BLOCK_PARAGRAPH);
                            parse_inlines(p, lline + sub_cs, ltlen - sub_cs);
                            md_block_add_child(si, p);
                        }
                        md_block_add_child(sublist, si);

                        /* Continue reading items at same or greater indent */
                        while (1) {
                            int sv2 = iter.pos;
                            const char *sl = line_iter_next(&iter, &line_len);
                            if (!sl) break;
                            int stlen = line_len;
                            while (stlen > 0 && sl[stlen - 1] == '\r') stlen--;
                            if (is_blank_line(sl, stlen)) { iter.pos = sv2; break; }
                            int sl_ind = count_leading_spaces(sl, stlen);
                            int sn2;
                            if (sl_ind >= ll && (is_unordered_list_item(sl, stlen, sl_ind)
                                || is_ordered_list_item(sl, stlen, sl_ind, &sn2))) {
                                int scs;
                                if (is_ordered_list_item(sl, stlen, sl_ind, &sn2)) {
                                    scs = sl_ind;
                                    while (scs < stlen && (sl[scs] >= '0' && sl[scs] <= '9')) scs++;
                                    scs++;
                                    if (scs < stlen && sl[scs] == ' ') scs++;
                                } else {
                                    scs = sl_ind + 2;
                                }
                                md_block_node_t *si2 = md_block_create(MD_BLOCK_LIST_ITEM);
                                if (scs < stlen) {
                                    md_block_node_t *p = md_block_create(MD_BLOCK_PARAGRAPH);
                                    parse_inlines(p, sl + scs, stlen - scs);
                                    md_block_add_child(si2, p);
                                }
                                md_block_add_child(sublist, si2);
                            } else {
                                iter.pos = sv2;
                                break;
                            }
                        }

                        md_block_add_child(last_item, sublist);
                    } else {
                        /* Same level — add as sibling */
                        int cs = ll + 2;
                        md_block_node_t *li = md_block_create(MD_BLOCK_LIST_ITEM);
                        li->level = 0;
                        if (cs < ltlen) {
                            md_block_node_t *p = md_block_create(MD_BLOCK_PARAGRAPH);
                            parse_inlines(p, lline + cs, ltlen - cs);
                            md_block_add_child(li, p);
                        }
                        md_block_add_child(list, li);
                        last_item = li;
                    }
                } else {
                    iter.pos = saved;
                    break;
                }
            }

            md_block_add_child(doc, list);
            continue;
        }

        /* Ordered list */
        int start_num;
        if (is_ordered_list_item(line, tlen, leading, &start_num)) {
            FLUSH_PARA();
            md_block_node_t *list = md_block_create(MD_BLOCK_LIST);
            list->ordered = 1;
            list->start = start_num;

            /* Find content start: skip digits, dot/paren, space */
            int cs = leading;
            while (cs < tlen && isdigit((unsigned char)line[cs])) cs++;
            cs++; /* skip . or ) */
            if (cs < tlen && line[cs] == ' ') cs++;

            md_block_node_t *item = md_block_create(MD_BLOCK_LIST_ITEM);
            item->level = leading / 2;
            if (cs < tlen) {
                md_block_node_t *p = md_block_create(MD_BLOCK_PARAGRAPH);
                parse_inlines(p, line + cs, tlen - cs);
                md_block_add_child(item, p);
            }
            md_block_add_child(list, item);

            /* Continue reading list items */
            while (1) {
                int saved = iter.pos;
                const char *lline = line_iter_next(&iter, &line_len);
                if (!lline) break;
                int ltlen = line_len;
                while (ltlen > 0 && lline[ltlen - 1] == '\r') ltlen--;

                if (is_blank_line(lline, ltlen)) break;

                int ll = count_leading_spaces(lline, ltlen);
                int sn;
                if (is_ordered_list_item(lline, ltlen, ll, &sn)) {
                    int ocs = ll;
                    while (ocs < ltlen && isdigit((unsigned char)lline[ocs])) ocs++;
                    ocs++; /* skip . or ) */
                    if (ocs < ltlen && lline[ocs] == ' ') ocs++;

                    md_block_node_t *li = md_block_create(MD_BLOCK_LIST_ITEM);
                    li->level = ll / 2;
                    if (ocs < ltlen) {
                        md_block_node_t *p = md_block_create(MD_BLOCK_PARAGRAPH);
                        parse_inlines(p, lline + ocs, ltlen - ocs);
                        md_block_add_child(li, p);
                    }
                    md_block_add_child(list, li);
                } else {
                    iter.pos = saved;
                    break;
                }
            }

            md_block_add_child(doc, list);
            continue;
        }

        /* Default: paragraph continuation */
        /* trim trailing \r from line for paragraph */
        PARA_APPEND(line, tlen);
    }

    FLUSH_PARA();
    free(para_buf);

    #undef FLUSH_PARA
    #undef PARA_APPEND

    return doc;
}
