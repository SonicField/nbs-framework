/*
 * test_md_highlight.c — Test suite for syntax highlighting engine.
 *
 * Tests language dispatch, per-language tokenisation, cross-line
 * context, and token-to-style mapping.
 *
 * Key invariants tested:
 *   1. find_lang returns non-NULL for all registered languages
 *   2. find_lang is case-insensitive
 *   3. find_lang returns NULL for unknown languages
 *   4. Tokeniser produces correct token types for language constructs
 *   5. Block comments carry context across lines
 *   6. Token styles match the palette defined in md_style.c
 *
 * Build:
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -I../nbs-common -o test_md_highlight test_md_highlight.c \
 *       md_highlight.c md_lang_c.c md_lang_js.c md_lang_py.c md_lang_pas.c \
 *       md_style.c ../nbs-common/nbs_term_attr.c \
 *       && ./test_md_highlight
 *
 * ASan:
 *   clang -fsanitize=address,undefined -g -O1 \
 *       -I../nbs-common -o test_md_highlight test_md_highlight.c \
 *       md_highlight.c md_lang_c.c md_lang_js.c md_lang_py.c md_lang_pas.c \
 *       md_style.c ../nbs-common/nbs_term_attr.c \
 *       && ./test_md_highlight
 */

#include "md_highlight.h"
#include "md_style.h"
#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Test infrastructure --- */

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-60s", #name); \
    fflush(stdout); \
    name(); \
    printf(" PASS\n"); \
    g_pass++; \
} while(0)

#define T_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        printf(" FAIL\n"); \
        fprintf(stderr, "  ASSERTION FAILED %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        g_fail++; \
        return; \
    } \
} while(0)

/* Maximum spans per line for test purposes */
#define MAX_SPANS 64

/* Helper: find a span with the given token type in the span array.
 * Returns the index of the first match, or -1 if not found. */
static int find_span_with_token(md_hl_span_t *spans, int count,
                                md_hl_token_t token) {
    for (int i = 0; i < count; i++) {
        if (spans[i].token == token) return i;
    }
    return -1;
}

/* Helper: check whether a specific substring is tagged with the
 * given token type. Verifies that the span covers exactly the
 * byte range [start, start+len) in the line. */
static int has_token_at(const char *line, md_hl_span_t *spans, int count,
                        const char *substr, md_hl_token_t token) {
    const char *p = strstr(line, substr);
    if (!p) return 0;
    int start = (int)(p - line);
    int len = (int)strlen(substr);
    for (int i = 0; i < count; i++) {
        if (spans[i].token == token &&
            spans[i].start == start &&
            spans[i].len == len) {
            return 1;
        }
    }
    return 0;
}

/* ================================================================
 * 1. LANGUAGE DISPATCH
 *
 * md_highlight_find_lang must find registered languages by
 * canonical name, alias, and case-insensitive variants.
 * Unknown languages must return NULL.
 * ================================================================ */

TEST(find_lang_c) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "find_lang('c') should return non-NULL");
    T_ASSERT(lang->tokenise != NULL, "C language should have a tokeniser");
}

TEST(find_lang_python) {
    const md_lang_t *lang = md_highlight_find_lang("python");
    T_ASSERT(lang != NULL, "find_lang('python') should return non-NULL");
    T_ASSERT(lang->tokenise != NULL, "Python language should have a tokeniser");
}

TEST(find_lang_case_insensitive) {
    const md_lang_t *lower = md_highlight_find_lang("python");
    const md_lang_t *upper = md_highlight_find_lang("Python");
    T_ASSERT(lower != NULL, "find_lang('python') should return non-NULL");
    T_ASSERT(upper != NULL, "find_lang('Python') should return non-NULL");
    T_ASSERT(lower == upper,
             "case-insensitive lookup should return the same pointer");
}

TEST(find_lang_unknown) {
    const md_lang_t *lang = md_highlight_find_lang("brainfuck");
    T_ASSERT(lang == NULL,
             "find_lang('brainfuck') should return NULL for unknown language");
}

TEST(find_lang_js_alias) {
    const md_lang_t *lang = md_highlight_find_lang("javascript");
    T_ASSERT(lang != NULL,
             "find_lang('javascript') should return non-NULL (alias for js)");
}

TEST(find_lang_pas) {
    const md_lang_t *lang = md_highlight_find_lang("pascal");
    T_ASSERT(lang != NULL,
             "find_lang('pascal') should return non-NULL");
    T_ASSERT(lang->tokenise != NULL, "Pascal language should have a tokeniser");
}

/* ================================================================
 * 2. C TOKENISATION
 *
 * The C tokeniser must identify keywords, strings, comments,
 * numbers, preprocessor directives, and types. Strings must
 * suppress keyword detection inside them.
 * ================================================================ */

TEST(tokenise_c_keyword) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "if (x)";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce at least 1 span, got %d", n);
    int idx = find_span_with_token(spans, n, MD_HL_KEYWORD);
    T_ASSERT(idx >= 0, "'if' should be tagged as KEYWORD");
    T_ASSERT(has_token_at(line, spans, n, "if", MD_HL_KEYWORD),
             "'if' keyword should span exactly bytes 0-2");
}

TEST(tokenise_c_string) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "\"hello\"";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce at least 1 span");
    int idx = find_span_with_token(spans, n, MD_HL_STRING);
    T_ASSERT(idx >= 0, "'\"hello\"' should contain a STRING token");
    /* The entire line is the string literal */
    T_ASSERT(spans[idx].start == 0 && spans[idx].len == 7,
             "STRING span should cover the entire literal, got start=%d len=%d",
             spans[idx].start, spans[idx].len);
}

TEST(tokenise_c_keyword_in_string) {
    /* Keywords inside string literals must NOT be tagged as KEYWORD.
     * Falsifier: if "if" inside the string produces an MD_HL_KEYWORD span. */
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "\"if this\"";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce at least 1 span");
    /* The entire line should be a single STRING span */
    int kw_idx = find_span_with_token(spans, n, MD_HL_KEYWORD);
    T_ASSERT(kw_idx < 0,
             "'if' inside a string must NOT be tagged as KEYWORD");
    int str_idx = find_span_with_token(spans, n, MD_HL_STRING);
    T_ASSERT(str_idx >= 0, "the string literal should be tagged as STRING");
}

TEST(tokenise_c_line_comment) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "// comment";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce at least 1 span");
    int idx = find_span_with_token(spans, n, MD_HL_COMMENT);
    T_ASSERT(idx >= 0, "'// comment' should be tagged as COMMENT");
    /* Comment should start at byte 0 and cover the entire line */
    T_ASSERT(spans[idx].start == 0,
             "comment span should start at 0, got %d", spans[idx].start);
    T_ASSERT(spans[idx].len == (int)strlen(line),
             "comment span should cover entire line, got len=%d", spans[idx].len);
}

TEST(tokenise_c_number) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "42";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce at least 1 span");
    int idx = find_span_with_token(spans, n, MD_HL_NUMBER);
    T_ASSERT(idx >= 0, "'42' should be tagged as NUMBER");
}

TEST(tokenise_c_preproc) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "#include";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce at least 1 span");
    int idx = find_span_with_token(spans, n, MD_HL_PREPROC);
    T_ASSERT(idx >= 0, "'#include' should be tagged as PREPROC");
}

TEST(tokenise_c_type) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "int x;";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce at least 1 span");
    int idx = find_span_with_token(spans, n, MD_HL_TYPE);
    T_ASSERT(idx >= 0, "'int' should be tagged as TYPE");
    T_ASSERT(has_token_at(line, spans, n, "int", MD_HL_TYPE),
             "'int' type should span exactly bytes 0-3");
}

/* ================================================================
 * 3. PYTHON TOKENISATION
 *
 * The Python tokeniser must identify keywords, comments, and
 * string literals with Python-specific delimiters.
 * ================================================================ */

TEST(tokenise_py_keyword) {
    const md_lang_t *lang = md_highlight_find_lang("python");
    T_ASSERT(lang != NULL, "Python language not found");

    const char *line = "def foo():";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce at least 1 span");
    int idx = find_span_with_token(spans, n, MD_HL_KEYWORD);
    T_ASSERT(idx >= 0, "'def' should be tagged as KEYWORD");
    T_ASSERT(has_token_at(line, spans, n, "def", MD_HL_KEYWORD),
             "'def' keyword should span exactly bytes 0-3");
}

TEST(tokenise_py_comment) {
    const md_lang_t *lang = md_highlight_find_lang("python");
    T_ASSERT(lang != NULL, "Python language not found");

    const char *line = "# comment";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce at least 1 span");
    int idx = find_span_with_token(spans, n, MD_HL_COMMENT);
    T_ASSERT(idx >= 0, "'# comment' should be tagged as COMMENT");
    T_ASSERT(spans[idx].start == 0,
             "comment should start at 0, got %d", spans[idx].start);
}

TEST(tokenise_py_string) {
    const md_lang_t *lang = md_highlight_find_lang("python");
    T_ASSERT(lang != NULL, "Python language not found");

    const char *line = "'hello'";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce at least 1 span");
    int idx = find_span_with_token(spans, n, MD_HL_STRING);
    T_ASSERT(idx >= 0, "'hello' should be tagged as STRING");
}

/* ================================================================
 * 4. CROSS-LINE STATE
 *
 * Block comments spanning multiple lines must carry context.
 * The tokeniser must update md_hl_context_t so the next line
 * continues in the correct state.
 * ================================================================ */

TEST(block_comment_spans_lines) {
    /* Feed a block-comment-open on line 1 — context should transition
     * to BLOCK_COMMENT. Then feed a block-comment-close on line 2 —
     * context should return to GROUND. */
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];

    /* Line 1: open block comment without closing */
    const char *line1 = "/* start";
    int n1 = lang->tokenise(line1, &ctx, spans, MAX_SPANS);
    T_ASSERT(n1 > 0, "line 1 should produce spans");
    T_ASSERT(ctx == MD_HL_CTX_BLOCK_COMMENT,
             "context should be BLOCK_COMMENT after unclosed /*, got %d", ctx);

    /* Verify line 1 is tagged as COMMENT */
    int idx = find_span_with_token(spans, n1, MD_HL_COMMENT);
    T_ASSERT(idx >= 0, "line 1 should contain a COMMENT span");

    /* Line 2: close the block comment */
    const char *line2 = "end */";
    int n2 = lang->tokenise(line2, &ctx, spans, MAX_SPANS);
    T_ASSERT(n2 > 0, "line 2 should produce spans");
    T_ASSERT(ctx == MD_HL_CTX_GROUND,
             "context should return to GROUND after */, got %d", ctx);
}

TEST(context_ground_initial) {
    /* A fresh context (zero-initialised) should be GROUND. */
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    T_ASSERT(ctx == 0, "MD_HL_CTX_GROUND should be 0, got %d", ctx);

    /* Also verify that a default-initialised struct member is GROUND */
    md_hl_context_t ctx2 = {0};
    T_ASSERT(ctx2 == MD_HL_CTX_GROUND,
             "zero-initialised context should be GROUND");
}

/* ================================================================
 * 5. TOKEN-TO-STYLE MAPPING
 *
 * md_highlight_token_style must return styles consistent with
 * the palette defined in md_style.c. These tests pin the exact
 * colour values to catch unintended palette changes.
 * ================================================================ */

TEST(token_style_keyword) {
    const nbs_style_t *s = md_highlight_token_style(MD_HL_KEYWORD);
    T_ASSERT(s != NULL, "token_style(KEYWORD) should return non-NULL");
    T_ASSERT(s->fg == 173,
             "KEYWORD fg should be 173 (terracotta), got %d", s->fg);
}

TEST(token_style_string) {
    const nbs_style_t *s = md_highlight_token_style(MD_HL_STRING);
    T_ASSERT(s != NULL, "token_style(STRING) should return non-NULL");
    T_ASSERT(s->fg == 108,
             "STRING fg should be 108 (sage), got %d", s->fg);
}

TEST(token_style_comment) {
    const nbs_style_t *s = md_highlight_token_style(MD_HL_COMMENT);
    T_ASSERT(s != NULL, "token_style(COMMENT) should return non-NULL");
    T_ASSERT((s->attrs & NBS_ATTR_DIM) != 0,
             "COMMENT style should have DIM attribute, got attrs=0x%x", s->attrs);
}

/* ================================================================
 * 6. ADDITIONAL TOKEN STYLE TESTS
 *
 * Pin remaining token styles to catch palette drift.
 * ================================================================ */

TEST(token_style_type) {
    const nbs_style_t *s = md_highlight_token_style(MD_HL_TYPE);
    T_ASSERT(s != NULL, "token_style(TYPE) should return non-NULL");
    T_ASSERT(s->fg == 110,
             "TYPE fg should be 110, got %d", s->fg);
}

TEST(token_style_number) {
    const nbs_style_t *s = md_highlight_token_style(MD_HL_NUMBER);
    T_ASSERT(s != NULL, "token_style(NUMBER) should return non-NULL");
    T_ASSERT(s->fg == 180,
             "NUMBER fg should be 180, got %d", s->fg);
}

TEST(token_style_preproc) {
    const nbs_style_t *s = md_highlight_token_style(MD_HL_PREPROC);
    T_ASSERT(s != NULL, "token_style(PREPROC) should return non-NULL");
    T_ASSERT(s->fg == 183,
             "PREPROC fg should be 183, got %d", s->fg);
}

TEST(token_style_normal) {
    /* NORMAL token style should map to code fence body style */
    const nbs_style_t *s = md_highlight_token_style(MD_HL_NORMAL);
    T_ASSERT(s != NULL, "token_style(NORMAL) should return non-NULL");
}

/* ================================================================
 * 7. JAVASCRIPT TOKENISATION
 *
 * async/await must be keywords. Template literals (backtick)
 * must be tagged as STRING.
 * ================================================================ */

TEST(tokenise_js_async_await) {
    const md_lang_t *lang = md_highlight_find_lang("js");
    T_ASSERT(lang != NULL, "JS language not found");

    const char *line = "async function foo() { await bar(); }";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "async", MD_HL_KEYWORD),
             "'async' should be tagged as KEYWORD");
    T_ASSERT(has_token_at(line, spans, n, "await", MD_HL_KEYWORD),
             "'await' should be tagged as KEYWORD");
}

TEST(tokenise_js_template_literal) {
    const md_lang_t *lang = md_highlight_find_lang("js");
    T_ASSERT(lang != NULL, "JS language not found");

    const char *line = "`hello world`";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    int idx = find_span_with_token(spans, n, MD_HL_STRING);
    T_ASSERT(idx >= 0, "template literal should be tagged as STRING");
    T_ASSERT(spans[idx].start == 0 && spans[idx].len == 13,
             "template literal should cover entire string, got start=%d len=%d",
             spans[idx].start, spans[idx].len);
}

TEST(tokenise_js_multiline_template_literal) {
    /* Backtick template literal spanning two lines */
    const md_lang_t *lang = md_highlight_find_lang("js");
    T_ASSERT(lang != NULL, "JS language not found");

    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];

    /* Line 1: open template literal without closing */
    const char *line1 = "const s = `hello";
    int n1 = lang->tokenise(line1, &ctx, spans, MAX_SPANS);
    T_ASSERT(n1 > 0, "line 1 should produce spans");
    T_ASSERT(ctx == MD_HL_CTX_STRING,
             "context should be STRING after unclosed backtick, got %d", ctx);

    /* Line 2: close the template literal */
    const char *line2 = "world`;";
    int n2 = lang->tokenise(line2, &ctx, spans, MAX_SPANS);
    T_ASSERT(n2 > 0, "line 2 should produce spans");
    T_ASSERT(ctx == MD_HL_CTX_GROUND,
             "context should return to GROUND after closing backtick, got %d", ctx);
}

/* ================================================================
 * 8. PASCAL TOKENISATION
 *
 * begin/end must be keywords. { } block comments must be detected.
 * Pascal keywords are case-insensitive.
 * ================================================================ */

TEST(tokenise_pas_begin_end) {
    const md_lang_t *lang = md_highlight_find_lang("pascal");
    T_ASSERT(lang != NULL, "Pascal language not found");

    const char *line = "begin end";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "begin", MD_HL_KEYWORD),
             "'begin' should be tagged as KEYWORD");
    T_ASSERT(has_token_at(line, spans, n, "end", MD_HL_KEYWORD),
             "'end' should be tagged as KEYWORD");
}

TEST(tokenise_pas_brace_comment) {
    const md_lang_t *lang = md_highlight_find_lang("pascal");
    T_ASSERT(lang != NULL, "Pascal language not found");

    const char *line = "{ this is a comment }";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    int idx = find_span_with_token(spans, n, MD_HL_COMMENT);
    T_ASSERT(idx >= 0, "'{ comment }' should be tagged as COMMENT");
    T_ASSERT(spans[idx].start == 0 && spans[idx].len == (int)strlen(line),
             "comment should cover entire line, got start=%d len=%d",
             spans[idx].start, spans[idx].len);
}

TEST(tokenise_pas_case_insensitive) {
    /* Pascal keywords are case-insensitive: BEGIN = begin */
    const md_lang_t *lang = md_highlight_find_lang("pascal");
    T_ASSERT(lang != NULL, "Pascal language not found");

    const char *line = "BEGIN END";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "BEGIN", MD_HL_KEYWORD),
             "'BEGIN' (uppercase) should be tagged as KEYWORD");
}

TEST(tokenise_pas_multiline_brace_comment) {
    /* Pascal { } comment spanning two lines */
    const md_lang_t *lang = md_highlight_find_lang("pascal");
    T_ASSERT(lang != NULL, "Pascal language not found");

    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];

    /* Line 1: open brace comment */
    const char *line1 = "{ start comment";
    int n1 = lang->tokenise(line1, &ctx, spans, MAX_SPANS);
    T_ASSERT(n1 > 0, "line 1 should produce spans");
    T_ASSERT(ctx == MD_HL_CTX_BLOCK_COMMENT,
             "context should be BLOCK_COMMENT after unclosed {, got %d", ctx);

    /* Line 2: close brace comment */
    const char *line2 = "end comment }";
    int n2 = lang->tokenise(line2, &ctx, spans, MAX_SPANS);
    T_ASSERT(n2 > 0, "line 2 should produce spans");
    T_ASSERT(ctx == MD_HL_CTX_GROUND,
             "context should return to GROUND after }, got %d", ctx);
}

/* ================================================================
 * 9. PYTHON TRIPLE-QUOTE CROSS-LINE
 *
 * Triple-quoted strings spanning multiple lines must correctly
 * carry context.
 * ================================================================ */

TEST(tokenise_py_triple_quote_spans_lines) {
    const md_lang_t *lang = md_highlight_find_lang("python");
    T_ASSERT(lang != NULL, "Python language not found");

    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];

    /* Line 1: open triple-quote without closing */
    const char *line1 = "s = \"\"\"hello";
    int n1 = lang->tokenise(line1, &ctx, spans, MAX_SPANS);
    T_ASSERT(n1 > 0, "line 1 should produce spans");
    T_ASSERT(ctx == MD_HL_CTX_STRING,
             "context should be STRING after unclosed \"\"\", got %d", ctx);

    /* Line 2: close triple-quote */
    const char *line2 = "world\"\"\"";
    int n2 = lang->tokenise(line2, &ctx, spans, MAX_SPANS);
    T_ASSERT(n2 > 0, "line 2 should produce spans");
    T_ASSERT(ctx == MD_HL_CTX_GROUND,
             "context should return to GROUND after closing \"\"\", got %d", ctx);

    /* Verify line 2 content is STRING */
    int idx = find_span_with_token(spans, n2, MD_HL_STRING);
    T_ASSERT(idx >= 0, "line 2 should contain a STRING span");
}

TEST(tokenise_py_class_keyword) {
    const md_lang_t *lang = md_highlight_find_lang("python");
    T_ASSERT(lang != NULL, "Python language not found");

    const char *line = "class MyClass:";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "class", MD_HL_KEYWORD),
             "'class' should be tagged as KEYWORD");
}

/* ================================================================
 * 10. C ADDITIONAL TESTS
 *
 * Hex numbers, floating-point numbers, block comments on one line,
 * void as type.
 * ================================================================ */

TEST(tokenise_c_hex_number) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "0xFF";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "0xFF", MD_HL_NUMBER),
             "'0xFF' should be tagged as NUMBER");
}

TEST(tokenise_c_float_number) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "3.14";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "3.14", MD_HL_NUMBER),
             "'3.14' should be tagged as NUMBER");
}

TEST(tokenise_c_block_comment_single_line) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "/* comment */";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    int idx = find_span_with_token(spans, n, MD_HL_COMMENT);
    T_ASSERT(idx >= 0, "'/* comment */' should be tagged as COMMENT");
    T_ASSERT(ctx == MD_HL_CTX_GROUND,
             "context should remain GROUND for single-line block comment, got %d", ctx);
}

TEST(tokenise_c_void_type) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "void *p;";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "void", MD_HL_TYPE),
             "'void' should be tagged as TYPE");
}

/* ================================================================
 * 11. EDGE CASES
 *
 * Empty line, whitespace-only line, keyword at end of line.
 * ================================================================ */

TEST(tokenise_empty_line) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n == 0, "empty line should produce 0 spans, got %d", n);
    T_ASSERT(ctx == MD_HL_CTX_GROUND,
             "context should remain GROUND for empty line, got %d", ctx);
}

TEST(tokenise_whitespace_only) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "    ";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n >= 0, "whitespace line should produce >= 0 spans");
    /* Should produce exactly 1 NORMAL span for whitespace */
    if (n > 0) {
        T_ASSERT(spans[0].token == MD_HL_NORMAL,
                 "whitespace should be tagged as NORMAL, got %d", spans[0].token);
    }
}

TEST(tokenise_keyword_at_end) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "x = return";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "return", MD_HL_KEYWORD),
             "'return' at end of line should be tagged as KEYWORD");
}

TEST(tokenise_keyword_at_start) {
    const md_lang_t *lang = md_highlight_find_lang("c");
    T_ASSERT(lang != NULL, "C language not found");

    const char *line = "while (1)";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "while", MD_HL_KEYWORD),
             "'while' at start of line should be tagged as KEYWORD");
}

/* ================================================================
 * 12. LANGUAGE DISPATCH ADDITIONAL
 *
 * C++, TypeScript, empty/null lookup.
 * ================================================================ */

TEST(find_lang_cpp) {
    const md_lang_t *lang = md_highlight_find_lang("cpp");
    T_ASSERT(lang != NULL, "find_lang('cpp') should return non-NULL");
    /* Also check alias */
    const md_lang_t *alias = md_highlight_find_lang("c++");
    T_ASSERT(alias != NULL, "find_lang('c++') should return non-NULL");
    T_ASSERT(lang == alias,
             "cpp and c++ should resolve to the same language");
}

TEST(find_lang_typescript) {
    const md_lang_t *lang = md_highlight_find_lang("ts");
    T_ASSERT(lang != NULL, "find_lang('ts') should return non-NULL");
    const md_lang_t *alias = md_highlight_find_lang("typescript");
    T_ASSERT(alias != NULL, "find_lang('typescript') should return non-NULL");
    T_ASSERT(lang == alias,
             "ts and typescript should resolve to the same language");
}

TEST(find_lang_null_and_empty) {
    const md_lang_t *null_lang = md_highlight_find_lang(NULL);
    T_ASSERT(null_lang == NULL, "find_lang(NULL) should return NULL");
    const md_lang_t *empty_lang = md_highlight_find_lang("");
    T_ASSERT(empty_lang == NULL, "find_lang(\"\") should return NULL");
}

/* ================================================================
 * 13. C++ TOKENISATION
 *
 * Verify C++ specific keywords (class, namespace, template)
 * are detected.
 * ================================================================ */

TEST(tokenise_cpp_keywords) {
    const md_lang_t *lang = md_highlight_find_lang("cpp");
    T_ASSERT(lang != NULL, "C++ language not found");

    const char *line = "class Foo : public Bar {};";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "class", MD_HL_KEYWORD),
             "'class' should be tagged as KEYWORD in C++");
    T_ASSERT(has_token_at(line, spans, n, "public", MD_HL_KEYWORD),
             "'public' should be tagged as KEYWORD in C++");
}

/* ================================================================
 * 14. TYPESCRIPT TOKENISATION
 *
 * Verify TS keywords like interface, type, readonly.
 * ================================================================ */

TEST(tokenise_ts_keywords) {
    const md_lang_t *lang = md_highlight_find_lang("typescript");
    T_ASSERT(lang != NULL, "TypeScript language not found");

    const char *line = "interface Foo { readonly x: number; }";
    md_hl_context_t ctx = MD_HL_CTX_GROUND;
    md_hl_span_t spans[MAX_SPANS];
    int n = lang->tokenise(line, &ctx, spans, MAX_SPANS);

    T_ASSERT(n > 0, "tokeniser should produce spans");
    T_ASSERT(has_token_at(line, spans, n, "interface", MD_HL_KEYWORD),
             "'interface' should be tagged as KEYWORD in TS");
    T_ASSERT(has_token_at(line, spans, n, "readonly", MD_HL_KEYWORD),
             "'readonly' should be tagged as KEYWORD in TS");
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(void) {
    printf("test_md_highlight\n");
    printf("=================\n\n");

    printf("Language dispatch:\n");
    RUN(find_lang_c);
    RUN(find_lang_python);
    RUN(find_lang_case_insensitive);
    RUN(find_lang_unknown);
    RUN(find_lang_js_alias);
    RUN(find_lang_pas);

    printf("\nC tokenisation:\n");
    RUN(tokenise_c_keyword);
    RUN(tokenise_c_string);
    RUN(tokenise_c_keyword_in_string);
    RUN(tokenise_c_line_comment);
    RUN(tokenise_c_number);
    RUN(tokenise_c_preproc);
    RUN(tokenise_c_type);

    printf("\nPython tokenisation:\n");
    RUN(tokenise_py_keyword);
    RUN(tokenise_py_comment);
    RUN(tokenise_py_string);

    printf("\nCross-line state:\n");
    RUN(block_comment_spans_lines);
    RUN(context_ground_initial);

    printf("\nToken-to-style mapping:\n");
    RUN(token_style_keyword);
    RUN(token_style_string);
    RUN(token_style_comment);
    RUN(token_style_type);
    RUN(token_style_number);
    RUN(token_style_preproc);
    RUN(token_style_normal);

    printf("\nJavaScript tokenisation:\n");
    RUN(tokenise_js_async_await);
    RUN(tokenise_js_template_literal);
    RUN(tokenise_js_multiline_template_literal);

    printf("\nPascal tokenisation:\n");
    RUN(tokenise_pas_begin_end);
    RUN(tokenise_pas_brace_comment);
    RUN(tokenise_pas_case_insensitive);
    RUN(tokenise_pas_multiline_brace_comment);

    printf("\nPython additional:\n");
    RUN(tokenise_py_triple_quote_spans_lines);
    RUN(tokenise_py_class_keyword);

    printf("\nC additional:\n");
    RUN(tokenise_c_hex_number);
    RUN(tokenise_c_float_number);
    RUN(tokenise_c_block_comment_single_line);
    RUN(tokenise_c_void_type);

    printf("\nEdge cases:\n");
    RUN(tokenise_empty_line);
    RUN(tokenise_whitespace_only);
    RUN(tokenise_keyword_at_end);
    RUN(tokenise_keyword_at_start);

    printf("\nLanguage dispatch additional:\n");
    RUN(find_lang_cpp);
    RUN(find_lang_typescript);
    RUN(find_lang_null_and_empty);

    printf("\nC++ tokenisation:\n");
    RUN(tokenise_cpp_keywords);

    printf("\nTypeScript tokenisation:\n");
    RUN(tokenise_ts_keywords);

    printf("\n=================\n");
    printf("%d passed, %d failed, %d total\n", g_pass, g_fail, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}
