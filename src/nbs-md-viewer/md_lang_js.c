/*
 * md_lang_js.c — JavaScript and TypeScript language definitions.
 *
 * Keyword and type arrays are sorted alphabetically for binary search.
 */

#include "md_highlight.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ── shared helpers (same as md_lang_c.c) ────────────────────────── */

static int js_starts_with(const char *s, int pos, int slen, const char *pfx) {
    int plen = (int)strlen(pfx);
    if (pos + plen > slen) return 0;
    return memcmp(s + pos, pfx, (size_t)plen) == 0;
}

static int js_kw_lookup(const char **table, const char *word, int wordlen) {
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

static int js_emit(md_hl_span_t *spans, int *n, int max,
                    int start, int length, md_hl_token_t tok) {
    if (*n >= max) return 0;
    spans[*n].start = start;
    spans[*n].len   = length;
    spans[*n].token = tok;
    (*n)++;
    return 1;
}

/* ── JS/TS generic tokeniser ─────────────────────────────────────── */

static int js_family_tokenise(const char *line, md_hl_context_t *ctx,
                               md_hl_span_t *spans, int max_spans,
                               const char **keywords, const char **types) {
    int slen = (int)strlen(line);
    int n = 0;
    int i = 0;

    /* Continue block comment */
    if (*ctx == MD_HL_CTX_BLOCK_COMMENT) {
        int found = -1;
        for (int j = 0; j + 2 <= slen; j++) {
            if (line[j] == '*' && line[j+1] == '/') { found = j; break; }
        }
        if (found >= 0) {
            int end = found + 2;
            if (!js_emit(spans, &n, max_spans, 0, end, MD_HL_COMMENT)) return n;
            *ctx = MD_HL_CTX_GROUND;
            i = end;
        } else {
            js_emit(spans, &n, max_spans, 0, slen, MD_HL_COMMENT);
            return n;
        }
    }

    /* Continue template literal (backtick multi-line) */
    if (*ctx == MD_HL_CTX_STRING) {
        int found = -1;
        for (int j = 0; j < slen; j++) {
            if (line[j] == '\\' && j + 1 < slen) { j++; continue; }
            if (line[j] == '`') { found = j; break; }
        }
        if (found >= 0) {
            int end = found + 1;
            if (!js_emit(spans, &n, max_spans, 0, end, MD_HL_STRING)) return n;
            *ctx = MD_HL_CTX_GROUND;
            i = end;
        } else {
            js_emit(spans, &n, max_spans, 0, slen, MD_HL_STRING);
            return n;
        }
    }

    /* Main scan */
    while (i < slen) {
        if (n >= max_spans) break;

        /* Whitespace */
        if (line[i] == ' ' || line[i] == '\t') {
            int start = i;
            while (i < slen && (line[i] == ' ' || line[i] == '\t')) i++;
            if (!js_emit(spans, &n, max_spans, start, i - start, MD_HL_NORMAL)) return n;
            continue;
        }

        /* Line comment */
        if (js_starts_with(line, i, slen, "//")) {
            js_emit(spans, &n, max_spans, i, slen - i, MD_HL_COMMENT);
            return n;
        }

        /* Block comment */
        if (js_starts_with(line, i, slen, "/*")) {
            int start = i;
            i += 2;
            int found = -1;
            for (int j = i; j + 2 <= slen; j++) {
                if (line[j] == '*' && line[j+1] == '/') { found = j; break; }
            }
            if (found >= 0) {
                int end = found + 2;
                if (!js_emit(spans, &n, max_spans, start, end - start, MD_HL_COMMENT)) return n;
                i = end;
            } else {
                *ctx = MD_HL_CTX_BLOCK_COMMENT;
                js_emit(spans, &n, max_spans, start, slen - start, MD_HL_COMMENT);
                return n;
            }
            continue;
        }

        /* Template literal (backtick) — can span lines */
        if (line[i] == '`') {
            int start = i;
            i++;
            int found = -1;
            for (int j = i; j < slen; j++) {
                if (line[j] == '\\' && j + 1 < slen) { j++; continue; }
                if (line[j] == '`') { found = j; break; }
            }
            if (found >= 0) {
                int end = found + 1;
                if (!js_emit(spans, &n, max_spans, start, end - start, MD_HL_STRING)) return n;
                i = end;
            } else {
                *ctx = MD_HL_CTX_STRING;
                js_emit(spans, &n, max_spans, start, slen - start, MD_HL_STRING);
                return n;
            }
            continue;
        }

        /* String literals */
        if (line[i] == '"' || line[i] == '\'') {
            char q = line[i];
            int start = i;
            i++;
            while (i < slen) {
                if (line[i] == '\\' && i + 1 < slen) { i += 2; continue; }
                if (line[i] == q) { i++; break; }
                i++;
            }
            if (!js_emit(spans, &n, max_spans, start, i - start, MD_HL_STRING)) return n;
            continue;
        }

        /* Numbers */
        if (isdigit((unsigned char)line[i]) ||
            (line[i] == '.' && i + 1 < slen && isdigit((unsigned char)line[i+1]))) {
            int start = i;
            if (line[i] == '0' && i + 1 < slen && (line[i+1] == 'x' || line[i+1] == 'X')) {
                i += 2;
                while (i < slen && isxdigit((unsigned char)line[i])) i++;
            } else {
                while (i < slen && isdigit((unsigned char)line[i])) i++;
                if (i < slen && line[i] == '.') {
                    i++;
                    while (i < slen && isdigit((unsigned char)line[i])) i++;
                }
                if (i < slen && (line[i] == 'e' || line[i] == 'E')) {
                    i++;
                    if (i < slen && (line[i] == '+' || line[i] == '-')) i++;
                    while (i < slen && isdigit((unsigned char)line[i])) i++;
                }
            }
            /* BigInt suffix */
            if (i < slen && line[i] == 'n') i++;
            if (!js_emit(spans, &n, max_spans, start, i - start, MD_HL_NUMBER)) return n;
            continue;
        }

        /* Identifiers / keywords / types */
        if (isalpha((unsigned char)line[i]) || line[i] == '_' || line[i] == '$') {
            int start = i;
            while (i < slen && (isalnum((unsigned char)line[i]) || line[i] == '_' || line[i] == '$')) i++;
            int wlen = i - start;
            md_hl_token_t tok = MD_HL_NORMAL;
            if (js_kw_lookup(keywords, line + start, wlen))     tok = MD_HL_KEYWORD;
            else if (js_kw_lookup(types, line + start, wlen))   tok = MD_HL_TYPE;
            if (!js_emit(spans, &n, max_spans, start, wlen, tok)) return n;
            continue;
        }

        /* Operator */
        if (!js_emit(spans, &n, max_spans, i, 1, MD_HL_OPERATOR)) return n;
        i++;
    }

    return n;
}

/* ── JavaScript ──────────────────────────────────────────────────── */

static const char *js_keywords[] = {
    "async", "await", "break", "case", "catch",
    "class", "const", "continue", "debugger", "default",
    "delete", "do", "else", "export", "extends",
    "false", "finally", "for", "from", "function",
    "if", "import", "in", "instanceof", "let",
    "new", "null", "of", "return", "super",
    "switch", "this", "throw", "true", "try",
    "typeof", "undefined", "var", "void", "while",
    "with", "yield", NULL
};

static const char *js_types[] = {
    "Array", "Boolean", "Date", "Error", "Function",
    "Map", "Number", "Object", "Promise", "RegExp",
    "Set", "String", "Symbol", "WeakMap", "WeakSet",
    NULL
};

static int js_tokenise(const char *line, md_hl_context_t *ctx,
                         md_hl_span_t *spans, int max_spans) {
    return js_family_tokenise(line, ctx, spans, max_spans,
                              js_keywords, js_types);
}

static const char *js_aliases[] = { "javascript", NULL };

const md_lang_t md_lang_js = {
    .name     = "js",
    .aliases  = js_aliases,
    .tokenise = js_tokenise
};

/* ── TypeScript ──────────────────────────────────────────────────── */

static const char *ts_keywords[] = {
    "abstract", "as", "async", "await", "break",
    "case", "catch", "class", "const", "continue",
    "debugger", "declare", "default", "delete", "do",
    "else", "enum", "export", "extends", "false",
    "finally", "for", "from", "function", "if",
    "implements", "import", "in", "instanceof", "interface",
    "is", "keyof", "let", "module", "namespace",
    "never", "new", "null", "of", "private",
    "protected", "public", "readonly", "return", "super",
    "switch", "this", "throw", "true", "try",
    "type", "typeof", "undefined", "unknown", "var",
    "void", "while", "with", "yield", NULL
};

static const char *ts_types[] = {
    "Array", "Boolean", "Date", "Error", "Function",
    "Map", "Number", "Object", "Promise", "RegExp",
    "Set", "String", "Symbol", "WeakMap", "WeakSet",
    "any", "bigint", "boolean", "never", "null",
    "number", "object", "string", "symbol", "undefined",
    "unknown", "void", NULL
};

static int ts_tokenise(const char *line, md_hl_context_t *ctx,
                         md_hl_span_t *spans, int max_spans) {
    return js_family_tokenise(line, ctx, spans, max_spans,
                              ts_keywords, ts_types);
}

static const char *ts_aliases[] = { "typescript", NULL };

const md_lang_t md_lang_ts = {
    .name     = "ts",
    .aliases  = ts_aliases,
    .tokenise = ts_tokenise
};
