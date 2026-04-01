/*
 * md_lang_py.c — Python language definition for syntax highlighting.
 *
 * Handles triple-quoted strings (""" and ''') as multi-line string
 * context, and # line comments.  No block comments or preprocessor.
 *
 * Keyword and type arrays are sorted alphabetically for binary search.
 * Special values (False, None, True) are in the keyword list.
 */

#include "md_highlight.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ── helpers ─────────────────────────────────────────────────────── */

static int py_kw_lookup(const char **table, const char *word, int wordlen) {
    if (!table) return 0;
    int lo = 0, hi = 0;
    while (table[hi]) hi++;
    hi--;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strncmp(word, table[mid], (size_t)wordlen);
        if (cmp == 0) {
            if (table[mid][wordlen] == '\0') return 1;
            cmp = -1;
        }
        if (cmp < 0) hi = mid - 1;
        else          lo = mid + 1;
    }
    return 0;
}

static int py_emit(md_hl_span_t *spans, int *n, int max,
                    int start, int length, md_hl_token_t tok) {
    if (*n >= max) return 0;
    spans[*n].start = start;
    spans[*n].len   = length;
    spans[*n].token = tok;
    (*n)++;
    return 1;
}

/* ── keyword and type tables ─────────────────────────────────────── */

static const char *py_keywords[] = {
    "False", "None", "True",
    "and", "as", "assert", "async", "await",
    "break", "class", "continue", "def", "del",
    "elif", "else", "except", "finally", "for",
    "from", "global", "if", "import", "in",
    "is", "lambda", "nonlocal", "not", "or",
    "pass", "raise", "return", "try", "while",
    "with", "yield", NULL
};

static const char *py_types[] = {
    "bool", "bytearray", "bytes", "complex", "dict",
    "float", "frozenset", "int", "list", "memoryview",
    "object", "range", "set", "str", "tuple", "type",
    NULL
};

/* Track which triple-quote delimiter is active.  Since md_hl_context_t
 * is a plain enum with no room for extra state, we use a file-scoped
 * variable.  This is fine because highlighting is single-threaded and
 * processes one code block at a time. */
static char py_triple_delim = '"';

/* ── tokeniser ───────────────────────────────────────────────────── */

static int py_tokenise(const char *line, md_hl_context_t *ctx,
                        md_hl_span_t *spans, int max_spans) {
    int slen = (int)strlen(line);
    int n = 0;
    int i = 0;

    /* ── continue multi-line triple-quoted string ───────────────── */
    if (*ctx == MD_HL_CTX_STRING) {
        char q = py_triple_delim;
        int found = -1;
        for (int j = 0; j + 3 <= slen; j++) {
            if (line[j] == '\\' && j + 1 < slen) { j++; continue; }
            if (line[j] == q && line[j+1] == q && line[j+2] == q) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            int end = found + 3;
            if (!py_emit(spans, &n, max_spans, 0, end, MD_HL_STRING)) return n;
            *ctx = MD_HL_CTX_GROUND;
            i = end;
        } else {
            py_emit(spans, &n, max_spans, 0, slen, MD_HL_STRING);
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
            if (!py_emit(spans, &n, max_spans, start, i - start, MD_HL_NORMAL)) return n;
            continue;
        }

        /* Line comment */
        if (line[i] == '#') {
            py_emit(spans, &n, max_spans, i, slen - i, MD_HL_COMMENT);
            return n;
        }

        /* String literals — check for triple-quote first */
        if (line[i] == '"' || line[i] == '\'') {
            char q = line[i];
            int start = i;

            /* Triple-quoted? */
            if (i + 2 < slen && line[i+1] == q && line[i+2] == q) {
                i += 3;
                /* Scan for closing triple-quote on same line */
                int found = -1;
                for (int j = i; j + 3 <= slen; j++) {
                    if (line[j] == '\\' && j + 1 < slen) { j++; continue; }
                    if (line[j] == q && line[j+1] == q && line[j+2] == q) {
                        found = j;
                        break;
                    }
                }
                if (found >= 0) {
                    int end = found + 3;
                    if (!py_emit(spans, &n, max_spans, start, end - start, MD_HL_STRING)) return n;
                    i = end;
                } else {
                    *ctx = MD_HL_CTX_STRING;
                    py_triple_delim = q;
                    py_emit(spans, &n, max_spans, start, slen - start, MD_HL_STRING);
                    return n;
                }
                continue;
            }

            /* Single/double quoted string (single line) */
            i++;
            while (i < slen) {
                if (line[i] == '\\' && i + 1 < slen) { i += 2; continue; }
                if (line[i] == q) { i++; break; }
                i++;
            }
            if (!py_emit(spans, &n, max_spans, start, i - start, MD_HL_STRING)) return n;
            continue;
        }

        /* Numbers */
        if (isdigit((unsigned char)line[i]) ||
            (line[i] == '.' && i + 1 < slen && isdigit((unsigned char)line[i+1]))) {
            int start = i;
            if (line[i] == '0' && i + 1 < slen && (line[i+1] == 'x' || line[i+1] == 'X')) {
                i += 2;
                while (i < slen && isxdigit((unsigned char)line[i])) i++;
            } else if (line[i] == '0' && i + 1 < slen && (line[i+1] == 'b' || line[i+1] == 'B')) {
                i += 2;
                while (i < slen && (line[i] == '0' || line[i] == '1' || line[i] == '_')) i++;
            } else if (line[i] == '0' && i + 1 < slen && (line[i+1] == 'o' || line[i+1] == 'O')) {
                i += 2;
                while (i < slen && line[i] >= '0' && line[i] <= '7') i++;
            } else {
                while (i < slen && (isdigit((unsigned char)line[i]) || line[i] == '_')) i++;
                if (i < slen && line[i] == '.') {
                    i++;
                    while (i < slen && (isdigit((unsigned char)line[i]) || line[i] == '_')) i++;
                }
                if (i < slen && (line[i] == 'e' || line[i] == 'E')) {
                    i++;
                    if (i < slen && (line[i] == '+' || line[i] == '-')) i++;
                    while (i < slen && isdigit((unsigned char)line[i])) i++;
                }
            }
            /* Python complex suffix */
            if (i < slen && (line[i] == 'j' || line[i] == 'J')) i++;
            if (!py_emit(spans, &n, max_spans, start, i - start, MD_HL_NUMBER)) return n;
            continue;
        }

        /* Decorators (treat as preprocessor-like) */
        if (line[i] == '@') {
            int start = i;
            i++;
            while (i < slen && (isalnum((unsigned char)line[i]) || line[i] == '_' || line[i] == '.')) i++;
            if (!py_emit(spans, &n, max_spans, start, i - start, MD_HL_PREPROC)) return n;
            continue;
        }

        /* Identifiers / keywords / types */
        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            int start = i;
            while (i < slen && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            int wlen = i - start;
            md_hl_token_t tok = MD_HL_NORMAL;
            if (py_kw_lookup(py_keywords, line + start, wlen))     tok = MD_HL_KEYWORD;
            else if (py_kw_lookup(py_types, line + start, wlen))   tok = MD_HL_TYPE;
            if (!py_emit(spans, &n, max_spans, start, wlen, tok)) return n;
            continue;
        }

        /* Operator */
        if (!py_emit(spans, &n, max_spans, i, 1, MD_HL_OPERATOR)) return n;
        i++;
    }

    return n;
}

/* ── language definition ─────────────────────────────────────────── */

static const char *py_aliases[] = { "python", NULL };

const md_lang_t md_lang_py = {
    .name     = "py",
    .aliases  = py_aliases,
    .tokenise = py_tokenise
};
