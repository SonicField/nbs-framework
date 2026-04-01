/*
 * md_lang_pas.c — Pascal language definition for syntax highlighting.
 *
 * Uses { } for block comments and // for line comments (Delphi/Free
 * Pascal extension).  Pascal keywords are case-insensitive, so the
 * tokeniser lower-cases identifiers before lookup.
 *
 * Keyword and type arrays are sorted alphabetically (lowercase)
 * for binary search.
 */

#include "md_highlight.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ── helpers ─────────────────────────────────────────────────────── */

static int pas_emit(md_hl_span_t *spans, int *n, int max,
                     int start, int length, md_hl_token_t tok) {
    if (*n >= max) return 0;
    spans[*n].start = start;
    spans[*n].len   = length;
    spans[*n].token = tok;
    (*n)++;
    return 1;
}

/* Case-insensitive binary search against a sorted lowercase table. */
static int pas_kw_lookup(const char **table, const char *word, int wordlen) {
    if (!table) return 0;

    /* Lower-case the word into a stack buffer */
    char buf[64];
    if (wordlen >= (int)sizeof(buf)) return 0;
    for (int k = 0; k < wordlen; k++)
        buf[k] = (char)tolower((unsigned char)word[k]);
    buf[wordlen] = '\0';

    int lo = 0, hi = 0;
    while (table[hi]) hi++;
    hi--;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strncmp(buf, table[mid], (size_t)wordlen);
        if (cmp == 0) {
            if (table[mid][wordlen] == '\0') return 1;
            cmp = -1;
        }
        if (cmp < 0) hi = mid - 1;
        else          lo = mid + 1;
    }
    return 0;
}

/* ── keyword and type tables (all lowercase, sorted) ─────────────── */

static const char *pas_keywords[] = {
    "and", "array", "begin", "case", "const",
    "div", "do", "downto", "else", "end",
    "file", "for", "function", "goto", "if",
    "in", "label", "mod", "nil", "not",
    "of", "or", "packed", "procedure", "program",
    "record", "repeat", "set", "then", "to",
    "type", "unit", "until", "uses", "var",
    "while", "with", NULL
};

static const char *pas_types[] = {
    "boolean", "byte", "char", "double", "extended",
    "integer", "longint", "pointer", "real", "shortint",
    "single", "string", "word", NULL
};

/* ── tokeniser ───────────────────────────────────────────────────── */

static int pas_tokenise(const char *line, md_hl_context_t *ctx,
                          md_hl_span_t *spans, int max_spans) {
    int slen = (int)strlen(line);
    int n = 0;
    int i = 0;

    /* ── continue block comment { ... } from previous line ──────── */
    if (*ctx == MD_HL_CTX_BLOCK_COMMENT) {
        int found = -1;
        for (int j = 0; j < slen; j++) {
            if (line[j] == '}') { found = j; break; }
        }
        if (found >= 0) {
            int end = found + 1;
            if (!pas_emit(spans, &n, max_spans, 0, end, MD_HL_COMMENT)) return n;
            *ctx = MD_HL_CTX_GROUND;
            i = end;
        } else {
            pas_emit(spans, &n, max_spans, 0, slen, MD_HL_COMMENT);
            return n;
        }
    }

    /* ── main scan ──────────────────────────────────────────────── */
    while (i < slen) {
        if (n >= max_spans) break;

        /* Whitespace */
        if (line[i] == ' ' || line[i] == '\t') {
            int start = i;
            while (i < slen && (line[i] == ' ' || line[i] == '\t')) i++;
            if (!pas_emit(spans, &n, max_spans, start, i - start, MD_HL_NORMAL)) return n;
            continue;
        }

        /* Line comment // */
        if (i + 1 < slen && line[i] == '/' && line[i+1] == '/') {
            pas_emit(spans, &n, max_spans, i, slen - i, MD_HL_COMMENT);
            return n;
        }

        /* Block comment { ... } */
        if (line[i] == '{') {
            int start = i;
            i++;
            int found = -1;
            for (int j = i; j < slen; j++) {
                if (line[j] == '}') { found = j; break; }
            }
            if (found >= 0) {
                int end = found + 1;
                if (!pas_emit(spans, &n, max_spans, start, end - start, MD_HL_COMMENT)) return n;
                i = end;
            } else {
                *ctx = MD_HL_CTX_BLOCK_COMMENT;
                pas_emit(spans, &n, max_spans, start, slen - start, MD_HL_COMMENT);
                return n;
            }
            continue;
        }

        /* Pascal string literals: 'hello world' (no backslash escaping;
         * embedded quote is doubled: 'it''s') */
        if (line[i] == '\'') {
            int start = i;
            i++;
            while (i < slen) {
                if (line[i] == '\'') {
                    i++;
                    /* Doubled quote → still inside string */
                    if (i < slen && line[i] == '\'') { i++; continue; }
                    break;
                }
                i++;
            }
            if (!pas_emit(spans, &n, max_spans, start, i - start, MD_HL_STRING)) return n;
            continue;
        }

        /* Numbers (including hex $FF) */
        if (isdigit((unsigned char)line[i]) || line[i] == '$') {
            int start = i;
            if (line[i] == '$') {
                i++;
                while (i < slen && isxdigit((unsigned char)line[i])) i++;
            } else {
                while (i < slen && isdigit((unsigned char)line[i])) i++;
                if (i < slen && line[i] == '.') {
                    /* Make sure it's not '..' range operator */
                    if (i + 1 < slen && line[i+1] == '.') {
                        /* Don't consume the dots */
                    } else {
                        i++;
                        while (i < slen && isdigit((unsigned char)line[i])) i++;
                    }
                }
                if (i < slen && (line[i] == 'e' || line[i] == 'E')) {
                    i++;
                    if (i < slen && (line[i] == '+' || line[i] == '-')) i++;
                    while (i < slen && isdigit((unsigned char)line[i])) i++;
                }
            }
            if (!pas_emit(spans, &n, max_spans, start, i - start, MD_HL_NUMBER)) return n;
            continue;
        }

        /* Identifiers / keywords / types (case-insensitive) */
        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            int start = i;
            while (i < slen && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            int wlen = i - start;
            md_hl_token_t tok = MD_HL_NORMAL;
            if (pas_kw_lookup(pas_keywords, line + start, wlen))     tok = MD_HL_KEYWORD;
            else if (pas_kw_lookup(pas_types, line + start, wlen))   tok = MD_HL_TYPE;
            if (!pas_emit(spans, &n, max_spans, start, wlen, tok)) return n;
            continue;
        }

        /* Operator */
        if (!pas_emit(spans, &n, max_spans, i, 1, MD_HL_OPERATOR)) return n;
        i++;
    }

    return n;
}

/* ── language definition ─────────────────────────────────────────── */

static const char *pas_aliases[] = { "pascal", "delphi", "honest", NULL };

const md_lang_t md_lang_pas = {
    .name     = "pas",
    .aliases  = pas_aliases,
    .tokenise = pas_tokenise
};
