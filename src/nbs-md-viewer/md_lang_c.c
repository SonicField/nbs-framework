/*
 * md_lang_c.c — C and C++ language definitions for syntax highlighting.
 *
 * Each language has a self-contained tokeniser with its own keyword
 * and type tables.  Arrays are sorted alphabetically for binary search.
 */

#include "md_highlight.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ── shared helpers ──────────────────────────────────────────────── */

/* Check whether the string at pos starts with prefix. */
static int starts_with(const char *s, int pos, int slen, const char *pfx) {
    int plen = (int)strlen(pfx);
    if (pos + plen > slen) return 0;
    return memcmp(s + pos, pfx, (size_t)plen) == 0;
}

/* Binary search a NULL-terminated sorted array of strings.
 * Returns 1 if word (of length wordlen) is found exactly. */
static int kw_lookup(const char **table, const char *word, int wordlen) {
    if (!table) return 0;
    int lo = 0, hi = 0;
    while (table[hi]) hi++;
    hi--;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strncmp(word, table[mid], (size_t)wordlen);
        if (cmp == 0) {
            if (table[mid][wordlen] == '\0') return 1;
            cmp = -1;  /* table entry is longer */
        }
        if (cmp < 0) hi = mid - 1;
        else          lo = mid + 1;
    }
    return 0;
}

/* Emit a span. Returns 1 on success, 0 if array is full. */
static int emit(md_hl_span_t *spans, int *n, int max,
                int start, int length, md_hl_token_t tok) {
    if (*n >= max) return 0;
    spans[*n].start = start;
    spans[*n].len   = length;
    spans[*n].token = tok;
    (*n)++;
    return 1;
}

/* ── generic C-family tokeniser ──────────────────────────────────── */

static int c_family_tokenise(const char *line, md_hl_context_t *ctx,
                              md_hl_span_t *spans, int max_spans,
                              const char **keywords, const char **types,
                              const char *line_comment,
                              const char *block_open, const char *block_close,
                              const char *preproc_prefix) {
    int slen = (int)strlen(line);
    int n = 0;
    int i = 0;

    /* ── continue block comment from previous line ──────────────── */
    if (*ctx == MD_HL_CTX_BLOCK_COMMENT && block_close) {
        int clen = (int)strlen(block_close);
        int found = -1;
        for (int j = 0; j + clen <= slen; j++) {
            if (memcmp(line + j, block_close, (size_t)clen) == 0) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            int end = found + clen;
            if (!emit(spans, &n, max_spans, 0, end, MD_HL_COMMENT)) return n;
            *ctx = MD_HL_CTX_GROUND;
            i = end;
        } else {
            emit(spans, &n, max_spans, 0, slen, MD_HL_COMMENT);
            return n;
        }
    }

    /* ── continue multi-line string from previous line ──────────── */
    if (*ctx == MD_HL_CTX_STRING) {
        /* For C-family languages, multi-line strings do not occur in
         * the traditional sense.  This state would only be entered by
         * languages that set it (like Python).  Just scan for closing
         * quote and reset. */
        emit(spans, &n, max_spans, 0, slen, MD_HL_STRING);
        *ctx = MD_HL_CTX_GROUND;
        return n;
    }

    /* ── preprocessor directive ─────────────────────────────────── */
    if (preproc_prefix) {
        int pp = 0;
        while (pp < slen && (line[pp] == ' ' || line[pp] == '\t')) pp++;
        if (starts_with(line, pp, slen, preproc_prefix)) {
            if (pp > 0)
                if (!emit(spans, &n, max_spans, 0, pp, MD_HL_NORMAL)) return n;
            emit(spans, &n, max_spans, pp, slen - pp, MD_HL_PREPROC);
            return n;
        }
    }

    /* ── main ground-state scan ─────────────────────────────────── */
    while (i < slen) {
        if (n >= max_spans) break;

        /* Whitespace */
        if (line[i] == ' ' || line[i] == '\t') {
            int start = i;
            while (i < slen && (line[i] == ' ' || line[i] == '\t')) i++;
            if (!emit(spans, &n, max_spans, start, i - start, MD_HL_NORMAL)) return n;
            continue;
        }

        /* Line comment */
        if (line_comment && starts_with(line, i, slen, line_comment)) {
            emit(spans, &n, max_spans, i, slen - i, MD_HL_COMMENT);
            return n;
        }

        /* Block comment open */
        if (block_open && starts_with(line, i, slen, block_open)) {
            int olen = (int)strlen(block_open);
            int clen = block_close ? (int)strlen(block_close) : 0;
            int start = i;
            i += olen;

            int found = -1;
            if (block_close) {
                for (int j = i; j + clen <= slen; j++) {
                    if (memcmp(line + j, block_close, (size_t)clen) == 0) {
                        found = j;
                        break;
                    }
                }
            }
            if (found >= 0) {
                int end = found + clen;
                if (!emit(spans, &n, max_spans, start, end - start, MD_HL_COMMENT)) return n;
                i = end;
            } else {
                *ctx = MD_HL_CTX_BLOCK_COMMENT;
                emit(spans, &n, max_spans, start, slen - start, MD_HL_COMMENT);
                return n;
            }
            continue;
        }

        /* String / char literals */
        if (line[i] == '"' || line[i] == '\'' || line[i] == '`') {
            char q = line[i];
            int start = i;
            i++;
            while (i < slen) {
                if (line[i] == '\\' && i + 1 < slen) { i += 2; continue; }
                if (line[i] == q) { i++; break; }
                i++;
            }
            if (!emit(spans, &n, max_spans, start, i - start, MD_HL_STRING)) return n;
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
            while (i < slen && (line[i] == 'u' || line[i] == 'U' ||
                                line[i] == 'l' || line[i] == 'L' ||
                                line[i] == 'f' || line[i] == 'F')) i++;
            if (!emit(spans, &n, max_spans, start, i - start, MD_HL_NUMBER)) return n;
            continue;
        }

        /* Identifiers / keywords / types */
        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            int start = i;
            while (i < slen && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            int wlen = i - start;
            md_hl_token_t tok = MD_HL_NORMAL;
            if (kw_lookup(keywords, line + start, wlen))     tok = MD_HL_KEYWORD;
            else if (kw_lookup(types, line + start, wlen))   tok = MD_HL_TYPE;
            if (!emit(spans, &n, max_spans, start, wlen, tok)) return n;
            continue;
        }

        /* Operator */
        if (!emit(spans, &n, max_spans, i, 1, MD_HL_OPERATOR)) return n;
        i++;
    }

    return n;
}

/* ── C language ──────────────────────────────────────────────────── */

static const char *c_keywords[] = {
    "auto", "break", "case", "const", "continue",
    "default", "do", "else", "enum", "extern",
    "for", "goto", "if", "inline", "register",
    "restrict", "return", "sizeof", "static", "struct",
    "switch", "typedef", "union", "volatile", "while",
    NULL
};

static const char *c_types[] = {
    "bool", "char", "double", "float",
    "int", "int16_t", "int32_t", "int64_t", "int8_t",
    "long", "short", "signed", "size_t",
    "uint16_t", "uint32_t", "uint64_t", "uint8_t",
    "unsigned", "void", NULL
};

static int c_tokenise(const char *line, md_hl_context_t *ctx,
                       md_hl_span_t *spans, int max_spans) {
    return c_family_tokenise(line, ctx, spans, max_spans,
                             c_keywords, c_types, "//", "/*", "*/", "#");
}

static const char *c_aliases[] = { "h", NULL };

const md_lang_t md_lang_c = {
    .name     = "c",
    .aliases  = c_aliases,
    .tokenise = c_tokenise
};

/* ── C++ language ────────────────────────────────────────────────── */

static const char *cpp_keywords[] = {
    "auto", "break", "case", "catch", "class",
    "const", "const_cast", "constexpr", "continue", "default",
    "delete", "do", "dynamic_cast", "else", "enum",
    "explicit", "export", "extern", "false", "final",
    "for", "friend", "goto", "if", "inline",
    "mutable", "namespace", "new", "noexcept", "nullptr",
    "operator", "override", "private", "protected", "public",
    "register", "reinterpret_cast", "restrict", "return", "sizeof",
    "static", "static_assert", "static_cast", "struct", "switch",
    "template", "this", "throw", "true", "try",
    "typedef", "typeid", "typename", "union", "using",
    "virtual", "volatile", "while", NULL
};

static const char *cpp_types[] = {
    "array", "bool", "char", "double", "float",
    "int", "int16_t", "int32_t", "int64_t", "int8_t",
    "long", "map", "optional", "pair", "set",
    "shared_ptr", "short", "signed", "size_t", "string",
    "tuple", "uint16_t", "uint32_t", "uint64_t", "uint8_t",
    "unique_ptr", "unsigned", "vector", "void", "wstring",
    NULL
};

static int cpp_tokenise(const char *line, md_hl_context_t *ctx,
                          md_hl_span_t *spans, int max_spans) {
    return c_family_tokenise(line, ctx, spans, max_spans,
                             cpp_keywords, cpp_types, "//", "/*", "*/", "#");
}

static const char *cpp_aliases[] = { "c++", "cc", "cxx", "hpp", NULL };

const md_lang_t md_lang_cpp = {
    .name     = "cpp",
    .aliases  = cpp_aliases,
    .tokenise = cpp_tokenise
};
