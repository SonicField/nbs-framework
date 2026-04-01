/*
 * test_md_parse.c — Test suite for the Markdown parser.
 *
 * Tests are organised by block type, then inline type, then edge cases.
 * Each test verifies the AST structure produced by md_parse().
 *
 * Plan §8.1 requires 30+ tests covering:
 *   - Each block type (paragraphs, headings, hrules, code fences,
 *     tables, lists, blockquotes)
 *   - Each inline type (text, bold, italic, bold+italic, code, link,
 *     softbreak, hardbreak)
 *   - Nesting and edge cases
 *   - Malformed input (must not crash)
 *
 * Build:
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -I../nbs-common -o test_md_parse test_md_parse.c md_parse.c md_ast.c \
 *       && ./test_md_parse
 */

#include "md_parse.h"
#include "md_ast.h"
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

/* Convenience: count block children */
static int count_block_children(md_block_node_t *parent) {
    int n = 0;
    for (md_block_node_t *c = parent->children; c; c = c->next) n++;
    return n;
}

/* Convenience: count inline children (used in inline nesting tests) */
static int count_inlines(md_inline_node_t *head) __attribute__((unused));
static int count_inlines(md_inline_node_t *head) {
    int n = 0;
    for (md_inline_node_t *c = head; c; c = c->next) n++;
    return n;
}

/* Convenience: get Nth block child (0-indexed) */
static md_block_node_t *nth_block(md_block_node_t *parent, int n) {
    md_block_node_t *c = parent->children;
    for (int i = 0; i < n && c; i++) c = c->next;
    return c;
}

/* Convenience: get Nth inline (0-indexed) */
static md_inline_node_t *nth_inline(md_inline_node_t *head, int n) __attribute__((unused));
static md_inline_node_t *nth_inline(md_inline_node_t *head, int n) {
    md_inline_node_t *c = head;
    for (int i = 0; i < n && c; i++) c = c->next;
    return c;
}

/* ================================================================
 * 1. EMPTY / MINIMAL INPUT
 * ================================================================ */

TEST(parse_empty_string) {
    md_block_node_t *doc = md_parse("");
    T_ASSERT(doc != NULL, "md_parse should never return NULL");
    T_ASSERT(doc->type == MD_BLOCK_DOCUMENT, "root should be DOCUMENT");
    T_ASSERT(doc->children == NULL, "empty input should produce no children");
    md_block_destroy(doc);
}

TEST(parse_whitespace_only) {
    md_block_node_t *doc = md_parse("   \n\n  \n");
    T_ASSERT(doc != NULL, "md_parse should never return NULL");
    T_ASSERT(doc->type == MD_BLOCK_DOCUMENT, "root should be DOCUMENT");
    /* Whitespace-only should produce no content blocks */
    md_block_destroy(doc);
}

TEST(parse_single_word) {
    md_block_node_t *doc = md_parse("Hello");
    T_ASSERT(doc != NULL, "md_parse should never return NULL");
    T_ASSERT(doc->children != NULL, "single word should produce a child");
    md_block_node_t *child = doc->children;
    T_ASSERT(child->type == MD_BLOCK_PARAGRAPH, "single word should be a paragraph");
    T_ASSERT(child->inlines != NULL, "paragraph should have inlines");
    T_ASSERT(child->inlines->type == MD_INLINE_TEXT, "first inline should be text");
    T_ASSERT(child->inlines->text != NULL, "text content should not be NULL");
    T_ASSERT(strstr(child->inlines->text, "Hello") != NULL,
             "text should contain 'Hello'");
    md_block_destroy(doc);
}

/* ================================================================
 * 2. HEADINGS (H1–H4)
 * ================================================================ */

TEST(parse_h1) {
    md_block_node_t *doc = md_parse("# Heading One");
    md_block_node_t *h = doc->children;
    T_ASSERT(h != NULL, "should have a child");
    T_ASSERT(h->type == MD_BLOCK_HEADING, "should be a heading");
    T_ASSERT(h->level == 1, "should be level 1, got %d", h->level);
    T_ASSERT(h->inlines != NULL, "heading should have inline text");
    T_ASSERT(strstr(h->inlines->text, "Heading One") != NULL,
             "heading text should contain 'Heading One'");
    md_block_destroy(doc);
}

TEST(parse_h2) {
    md_block_node_t *doc = md_parse("## Heading Two");
    md_block_node_t *h = doc->children;
    T_ASSERT(h != NULL, "should have a child");
    T_ASSERT(h->type == MD_BLOCK_HEADING, "should be a heading");
    T_ASSERT(h->level == 2, "should be level 2, got %d", h->level);
    md_block_destroy(doc);
}

TEST(parse_h3) {
    md_block_node_t *doc = md_parse("### Heading Three");
    md_block_node_t *h = doc->children;
    T_ASSERT(h != NULL, "should have a child");
    T_ASSERT(h->type == MD_BLOCK_HEADING, "should be a heading");
    T_ASSERT(h->level == 3, "should be level 3, got %d", h->level);
    md_block_destroy(doc);
}

TEST(parse_h4) {
    md_block_node_t *doc = md_parse("#### Heading Four");
    md_block_node_t *h = doc->children;
    T_ASSERT(h != NULL, "should have a child");
    T_ASSERT(h->type == MD_BLOCK_HEADING, "should be a heading");
    T_ASSERT(h->level == 4, "should be level 4, got %d", h->level);
    md_block_destroy(doc);
}

TEST(parse_h5_treated_as_h4) {
    /* Plan §3.1 only supports levels 1-4. ##### should either
     * be treated as H4 or as a paragraph. Either is acceptable. */
    md_block_node_t *doc = md_parse("##### Five Hashes");
    T_ASSERT(doc != NULL, "should not return NULL");
    /* Must not crash — that's the key assertion */
    md_block_destroy(doc);
}

/* ================================================================
 * 3. HORIZONTAL RULES
 * ================================================================ */

TEST(parse_hrule_dashes) {
    md_block_node_t *doc = md_parse("---");
    md_block_node_t *child = doc->children;
    T_ASSERT(child != NULL, "should have a child");
    T_ASSERT(child->type == MD_BLOCK_HRULE, "--- should be an hrule");
    md_block_destroy(doc);
}

TEST(parse_hrule_stars) {
    md_block_node_t *doc = md_parse("***");
    md_block_node_t *child = doc->children;
    T_ASSERT(child != NULL, "should have a child");
    T_ASSERT(child->type == MD_BLOCK_HRULE, "*** should be an hrule");
    md_block_destroy(doc);
}

TEST(parse_hrule_underscores) {
    md_block_node_t *doc = md_parse("___");
    md_block_node_t *child = doc->children;
    T_ASSERT(child != NULL, "should have a child");
    T_ASSERT(child->type == MD_BLOCK_HRULE, "___ should be an hrule");
    md_block_destroy(doc);
}

TEST(parse_hrule_many_chars) {
    md_block_node_t *doc = md_parse("----------");
    md_block_node_t *child = doc->children;
    T_ASSERT(child != NULL, "should have a child");
    T_ASSERT(child->type == MD_BLOCK_HRULE, "---------- should be an hrule");
    md_block_destroy(doc);
}

/* ================================================================
 * 4. PARAGRAPHS
 * ================================================================ */

TEST(parse_single_paragraph) {
    md_block_node_t *doc = md_parse("This is a paragraph.");
    md_block_node_t *child = doc->children;
    T_ASSERT(child != NULL, "should have a child");
    T_ASSERT(child->type == MD_BLOCK_PARAGRAPH, "should be a paragraph");
    T_ASSERT(child->inlines != NULL, "paragraph should have inline content");
    md_block_destroy(doc);
}

TEST(parse_two_paragraphs) {
    md_block_node_t *doc = md_parse("Para one.\n\nPara two.");
    int n = count_block_children(doc);
    T_ASSERT(n == 2, "should have 2 children, got %d", n);
    md_block_node_t *p1 = nth_block(doc, 0);
    md_block_node_t *p2 = nth_block(doc, 1);
    T_ASSERT(p1->type == MD_BLOCK_PARAGRAPH, "first should be paragraph");
    T_ASSERT(p2->type == MD_BLOCK_PARAGRAPH, "second should be paragraph");
    md_block_destroy(doc);
}

TEST(parse_multiline_paragraph) {
    /* Continuation lines (no blank between) should merge into one paragraph. */
    md_block_node_t *doc = md_parse("Line one\nLine two\nLine three");
    int n = count_block_children(doc);
    T_ASSERT(n == 1, "continuation lines should produce 1 paragraph, got %d", n);
    md_block_node_t *p = doc->children;
    T_ASSERT(p->type == MD_BLOCK_PARAGRAPH, "should be a paragraph");
    md_block_destroy(doc);
}

/* ================================================================
 * 5. CODE FENCES
 * ================================================================ */

TEST(parse_code_fence_basic) {
    md_block_node_t *doc = md_parse("```\ncode here\n```");
    md_block_node_t *child = doc->children;
    T_ASSERT(child != NULL, "should have a child");
    T_ASSERT(child->type == MD_BLOCK_CODE_FENCE, "should be code fence");
    T_ASSERT(child->raw != NULL, "code fence should have raw content");
    T_ASSERT(strstr(child->raw, "code here") != NULL,
             "raw should contain 'code here'");
    md_block_destroy(doc);
}

TEST(parse_code_fence_with_language) {
    md_block_node_t *doc = md_parse("```c\nint x = 42;\n```");
    md_block_node_t *child = doc->children;
    T_ASSERT(child != NULL, "should have a child");
    T_ASSERT(child->type == MD_BLOCK_CODE_FENCE, "should be code fence");
    T_ASSERT(child->language != NULL, "should have language tag");
    T_ASSERT(strcmp(child->language, "c") == 0,
             "language should be 'c', got '%s'", child->language);
    md_block_destroy(doc);
}

TEST(parse_code_fence_no_inline_parse) {
    /* Inline delimiters inside code fences must NOT be parsed.
     * Plan §4.1: inline parser SHALL NOT parse inside code fences.
     * Falsifier: if **bold** inside a fence produces an MD_INLINE_BOLD node. */
    md_block_node_t *doc = md_parse("```\n**not bold** `not code`\n```");
    md_block_node_t *child = doc->children;
    T_ASSERT(child != NULL, "should have a child");
    T_ASSERT(child->type == MD_BLOCK_CODE_FENCE, "should be code fence");
    T_ASSERT(child->raw != NULL, "should have raw content");
    T_ASSERT(strstr(child->raw, "**not bold**") != NULL,
             "raw should contain literal ** delimiters");
    /* Code fences must not have inline children */
    T_ASSERT(child->inlines == NULL,
             "code fence must not have inline nodes (no parsing inside fences)");
    md_block_destroy(doc);
}

TEST(parse_code_fence_unclosed) {
    /* Unclosed code fence: must not crash. Plan §4.3 says unrecognised
     * constructs are treated as plain text. An unclosed fence can
     * consume everything to EOF — that's acceptable behaviour. */
    md_block_node_t *doc = md_parse("```\nunclosed code");
    T_ASSERT(doc != NULL, "must not return NULL");
    /* Must not crash — that's the key assertion */
    md_block_destroy(doc);
}

/* ================================================================
 * 6. TABLES
 * ================================================================ */

TEST(parse_simple_table) {
    const char *input =
        "| A | B | C |\n"
        "|---|---|---|\n"
        "| 1 | 2 | 3 |\n";
    md_block_node_t *doc = md_parse(input);
    md_block_node_t *table = doc->children;
    T_ASSERT(table != NULL, "should have a child");
    T_ASSERT(table->type == MD_BLOCK_TABLE, "should be a table");
    T_ASSERT(table->col_count == 3, "should have 3 columns, got %d", table->col_count);
    /* Should have at least header row + one body row */
    int rows = count_block_children(table);
    T_ASSERT(rows >= 2, "should have >= 2 rows, got %d", rows);
    md_block_destroy(doc);
}

TEST(parse_table_alignment) {
    const char *input =
        "| Left | Centre | Right |\n"
        "|:-----|:------:|------:|\n"
        "| a    | b      | c     |\n";
    md_block_node_t *doc = md_parse(input);
    md_block_node_t *table = doc->children;
    T_ASSERT(table != NULL, "should have a child");
    T_ASSERT(table->type == MD_BLOCK_TABLE, "should be a table");
    T_ASSERT(table->col_align != NULL, "should have column alignments");
    T_ASSERT(table->col_align[0] == MD_ALIGN_LEFT,
             "col 0 should be LEFT, got %d", table->col_align[0]);
    T_ASSERT(table->col_align[1] == MD_ALIGN_CENTRE,
             "col 1 should be CENTRE, got %d", table->col_align[1]);
    T_ASSERT(table->col_align[2] == MD_ALIGN_RIGHT,
             "col 2 should be RIGHT, got %d", table->col_align[2]);
    md_block_destroy(doc);
}

TEST(parse_table_header_row_flagged) {
    const char *input =
        "| H1 | H2 |\n"
        "|----|----|  \n"
        "| d1 | d2 |\n";
    md_block_node_t *doc = md_parse(input);
    md_block_node_t *table = doc->children;
    T_ASSERT(table != NULL && table->type == MD_BLOCK_TABLE, "should be a table");
    md_block_node_t *first_row = table->children;
    T_ASSERT(first_row != NULL, "table should have rows");
    T_ASSERT(first_row->type == MD_BLOCK_TABLE_ROW, "first child should be a row");
    T_ASSERT(first_row->is_header == 1, "first row should be header");
    if (first_row->next) {
        T_ASSERT(first_row->next->is_header == 0, "second row should NOT be header");
    }
    md_block_destroy(doc);
}

/* ================================================================
 * 7. LISTS
 * ================================================================ */

TEST(parse_unordered_list) {
    const char *input = "- Item one\n- Item two\n- Item three\n";
    md_block_node_t *doc = md_parse(input);
    md_block_node_t *list = doc->children;
    T_ASSERT(list != NULL, "should have a child");
    T_ASSERT(list->type == MD_BLOCK_LIST, "should be a list");
    T_ASSERT(list->ordered == 0, "should be unordered");
    int items = count_block_children(list);
    T_ASSERT(items == 3, "should have 3 items, got %d", items);
    md_block_destroy(doc);
}

TEST(parse_ordered_list) {
    const char *input = "1. First\n2. Second\n3. Third\n";
    md_block_node_t *doc = md_parse(input);
    md_block_node_t *list = doc->children;
    T_ASSERT(list != NULL, "should have a child");
    T_ASSERT(list->type == MD_BLOCK_LIST, "should be a list");
    T_ASSERT(list->ordered == 1, "should be ordered");
    T_ASSERT(list->start == 1, "start should be 1, got %d", list->start);
    int items = count_block_children(list);
    T_ASSERT(items == 3, "should have 3 items, got %d", items);
    md_block_destroy(doc);
}

TEST(parse_nested_list) {
    /* Plan §3.1: ListItem has block children[] which MAY contain nested lists.
     * This is the hierarchical model: List -> ListItem -> List -> ListItem.
     * The parser MUST produce this structure for mixed list types and
     * blockquote-inside-list to work correctly (theologian analysis). */
    const char *input =
        "- Outer\n"
        "  - Inner one\n"
        "  - Inner two\n"
        "- Outer again\n";
    md_block_node_t *doc = md_parse(input);
    md_block_node_t *list = doc->children;
    T_ASSERT(list != NULL, "should have a list");
    T_ASSERT(list->type == MD_BLOCK_LIST, "should be a list");
    /* Outer list should have 2 items: "Outer" and "Outer again" */
    md_block_node_t *item1 = list->children;
    T_ASSERT(item1 != NULL, "should have first item");
    T_ASSERT(item1->type == MD_BLOCK_LIST_ITEM, "should be list item");
    /* First item should contain a nested list as a child block */
    int found_sublist = 0;
    for (md_block_node_t *c = item1->children; c; c = c->next) {
        if (c->type == MD_BLOCK_LIST) {
            found_sublist = 1;
            /* Nested list should have 2 items */
            int inner_count = count_block_children(c);
            T_ASSERT(inner_count == 2,
                     "nested list should have 2 items, got %d", inner_count);
        }
    }
    T_ASSERT(found_sublist,
             "first item should contain a nested list (hierarchical model)");
    md_block_destroy(doc);
}

TEST(parse_ordered_list_start_number) {
    const char *input = "5. Fifth\n6. Sixth\n";
    md_block_node_t *doc = md_parse(input);
    md_block_node_t *list = doc->children;
    T_ASSERT(list != NULL && list->type == MD_BLOCK_LIST, "should be a list");
    T_ASSERT(list->start == 5, "start should be 5, got %d", list->start);
    md_block_destroy(doc);
}

/* ================================================================
 * 8. BLOCKQUOTES
 * ================================================================ */

TEST(parse_blockquote) {
    md_block_node_t *doc = md_parse("> Quoted text here");
    md_block_node_t *bq = doc->children;
    T_ASSERT(bq != NULL, "should have a child");
    T_ASSERT(bq->type == MD_BLOCK_BLOCKQUOTE, "should be a blockquote");
    T_ASSERT(bq->children != NULL, "blockquote should have children");
    md_block_destroy(doc);
}

TEST(parse_nested_blockquote) {
    md_block_node_t *doc = md_parse("> Outer\n> > Inner");
    md_block_node_t *bq = doc->children;
    T_ASSERT(bq != NULL && bq->type == MD_BLOCK_BLOCKQUOTE, "should be blockquote");
    /* Look for nested blockquote */
    int found_inner = 0;
    for (md_block_node_t *c = bq->children; c; c = c->next) {
        if (c->type == MD_BLOCK_BLOCKQUOTE) found_inner = 1;
    }
    T_ASSERT(found_inner, "should contain a nested blockquote");
    md_block_destroy(doc);
}

/* ================================================================
 * 9. INLINE PARSING — BOLD, ITALIC, CODE, LINK
 * ================================================================ */

TEST(parse_inline_bold) {
    md_block_node_t *doc = md_parse("This is **bold** text");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    /* Find a BOLD inline node */
    int found_bold = 0;
    for (md_inline_node_t *inl = para->inlines; inl; inl = inl->next) {
        if (inl->type == MD_INLINE_BOLD) found_bold = 1;
    }
    T_ASSERT(found_bold, "should contain a bold inline node");
    md_block_destroy(doc);
}

TEST(parse_inline_italic) {
    md_block_node_t *doc = md_parse("This is *italic* text");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    int found_italic = 0;
    for (md_inline_node_t *inl = para->inlines; inl; inl = inl->next) {
        if (inl->type == MD_INLINE_ITALIC) found_italic = 1;
    }
    T_ASSERT(found_italic, "should contain an italic inline node");
    md_block_destroy(doc);
}

TEST(parse_inline_bold_italic) {
    md_block_node_t *doc = md_parse("This is ***bold italic*** text");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    int found_bi = 0;
    for (md_inline_node_t *inl = para->inlines; inl; inl = inl->next) {
        if (inl->type == MD_INLINE_BOLD_ITALIC) found_bi = 1;
    }
    T_ASSERT(found_bi, "should contain a bold+italic inline node");
    md_block_destroy(doc);
}

TEST(parse_inline_code) {
    md_block_node_t *doc = md_parse("Use `printf()` here");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    int found_code = 0;
    for (md_inline_node_t *inl = para->inlines; inl; inl = inl->next) {
        if (inl->type == MD_INLINE_CODE) {
            found_code = 1;
            T_ASSERT(inl->text != NULL, "code span should have text");
            T_ASSERT(strcmp(inl->text, "printf()") == 0,
                     "code text should be 'printf()', got '%s'", inl->text);
        }
    }
    T_ASSERT(found_code, "should contain an inline code node");
    md_block_destroy(doc);
}

TEST(parse_inline_link) {
    md_block_node_t *doc = md_parse("Click [here](https://example.com) now");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    int found_link = 0;
    for (md_inline_node_t *inl = para->inlines; inl; inl = inl->next) {
        if (inl->type == MD_INLINE_LINK) {
            found_link = 1;
            T_ASSERT(inl->url != NULL, "link should have URL");
            T_ASSERT(strcmp(inl->url, "https://example.com") == 0,
                     "URL should be 'https://example.com', got '%s'", inl->url);
            /* Link should have display text as child */
            T_ASSERT(inl->children != NULL, "link should have display text children");
        }
    }
    T_ASSERT(found_link, "should contain a link inline node");
    md_block_destroy(doc);
}

TEST(parse_inline_code_no_inner_parse) {
    /* Inline delimiters inside code spans must NOT be parsed.
     * Plan §4.1: inline parser shall NOT parse inside code spans.
     * Falsifier: if **bold** inside `code` produces MD_INLINE_BOLD. */
    md_block_node_t *doc = md_parse("Look at `**not bold**` here");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    for (md_inline_node_t *inl = para->inlines; inl; inl = inl->next) {
        if (inl->type == MD_INLINE_CODE) {
            T_ASSERT(inl->text != NULL, "code should have text");
            T_ASSERT(strstr(inl->text, "**not bold**") != NULL,
                     "code span should contain literal ** delimiters");
        }
        T_ASSERT(inl->type != MD_INLINE_BOLD,
                 "** inside code span must NOT produce BOLD node");
    }
    md_block_destroy(doc);
}

/* ================================================================
 * 10. DOCUMENT STRUCTURE — MIXED BLOCKS
 * ================================================================ */

TEST(parse_heading_then_paragraph) {
    md_block_node_t *doc = md_parse("# Title\n\nBody text here.");
    int n = count_block_children(doc);
    T_ASSERT(n == 2, "should have 2 blocks, got %d", n);
    md_block_node_t *h = nth_block(doc, 0);
    md_block_node_t *p = nth_block(doc, 1);
    T_ASSERT(h->type == MD_BLOCK_HEADING, "first should be heading");
    T_ASSERT(p->type == MD_BLOCK_PARAGRAPH, "second should be paragraph");
    md_block_destroy(doc);
}

TEST(parse_heading_hrule_paragraph) {
    md_block_node_t *doc = md_parse("# Title\n\n---\n\nBody text.");
    int n = count_block_children(doc);
    T_ASSERT(n == 3, "should have 3 blocks, got %d", n);
    T_ASSERT(nth_block(doc, 0)->type == MD_BLOCK_HEADING, "first: heading");
    T_ASSERT(nth_block(doc, 1)->type == MD_BLOCK_HRULE, "second: hrule");
    T_ASSERT(nth_block(doc, 2)->type == MD_BLOCK_PARAGRAPH, "third: paragraph");
    md_block_destroy(doc);
}

TEST(parse_complex_document) {
    const char *input =
        "# Main Title\n\n"
        "Introduction paragraph.\n\n"
        "## Section One\n\n"
        "Some text here.\n\n"
        "---\n\n"
        "### Subsection\n\n"
        "More text.\n";
    md_block_node_t *doc = md_parse(input);
    int n = count_block_children(doc);
    T_ASSERT(n >= 6, "complex doc should have >= 6 blocks, got %d", n);
    md_block_destroy(doc);
}

/* ================================================================
 * 11. EDGE CASES AND MALFORMED INPUT
 * ================================================================ */

TEST(parse_null_input) {
    /* md_parse(NULL) — should handle gracefully */
    md_block_node_t *doc = md_parse(NULL);
    /* Either returns an empty doc or never crashes */
    if (doc) {
        T_ASSERT(doc->type == MD_BLOCK_DOCUMENT, "should be DOCUMENT");
        md_block_destroy(doc);
    }
}

TEST(parse_binary_garbage) {
    /* Must not crash on binary input. Plan §4.3: unrecognised
     * constructs are treated as plain text. */
    char garbage[64];
    for (int i = 0; i < 63; i++) garbage[i] = (char)(i + 1);
    garbage[63] = '\0';
    md_block_node_t *doc = md_parse(garbage);
    T_ASSERT(doc != NULL, "must not return NULL on binary input");
    md_block_destroy(doc);
}

TEST(parse_very_long_line) {
    /* A single line of 10000 characters. Must not crash. */
    char *long_line = malloc(10001);
    T_ASSERT(long_line != NULL, "malloc failed");
    memset(long_line, 'x', 10000);
    long_line[10000] = '\0';
    md_block_node_t *doc = md_parse(long_line);
    T_ASSERT(doc != NULL, "must not return NULL on long line");
    md_block_destroy(doc);
    free(long_line);
}

TEST(parse_many_blank_lines) {
    /* Many consecutive blank lines should not produce excessive nodes. */
    md_block_node_t *doc = md_parse("\n\n\n\n\n\n\n\n\n\n");
    T_ASSERT(doc != NULL, "must not return NULL");
    md_block_destroy(doc);
}

TEST(parse_heading_no_space) {
    /* "#NoSpace" — per strict CommonMark, # without space is NOT a heading.
     * Parser may treat it as heading or paragraph. Must not crash. */
    md_block_node_t *doc = md_parse("#NoSpace");
    T_ASSERT(doc != NULL, "must not return NULL");
    /* Accept either interpretation — just don't crash */
    md_block_destroy(doc);
}

TEST(parse_mixed_list_markers) {
    /* Different markers in sequence. Should still produce valid list structure. */
    const char *input = "- dash\n* star\n+ plus\n";
    md_block_node_t *doc = md_parse(input);
    T_ASSERT(doc != NULL, "must not return NULL");
    /* Should produce at least one list */
    md_block_node_t *child = doc->children;
    T_ASSERT(child != NULL, "should have children");
    md_block_destroy(doc);
}

/* ================================================================
 * 12. INLINE NESTING
 * ================================================================ */

TEST(parse_bold_inside_italic) {
    /* *italic **bold inside** italic* — tests nested inline formatting.
     * Plan §4.1: inline parser shall handle nested formatting. */
    md_block_node_t *doc = md_parse("*italic **bold** italic*");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    /* Check that we have some inline structure — exact shape may vary */
    T_ASSERT(para->inlines != NULL, "should have inline content");
    md_block_destroy(doc);
}

TEST(parse_link_with_bold_text) {
    md_block_node_t *doc = md_parse("[**bold link**](http://x.com)");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    int found_link = 0;
    for (md_inline_node_t *inl = para->inlines; inl; inl = inl->next) {
        if (inl->type == MD_INLINE_LINK) found_link = 1;
    }
    T_ASSERT(found_link, "should contain a link");
    md_block_destroy(doc);
}

/* ================================================================
 * 13. SOFT AND HARD BREAKS
 * ================================================================ */

TEST(parse_softbreak) {
    /* Adjacent lines without blank between produce soft break. */
    md_block_node_t *doc = md_parse("Line one\nLine two");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    /* Should have inline content that spans both lines, possibly with softbreak */
    T_ASSERT(para->inlines != NULL, "should have inlines");
    md_block_destroy(doc);
}

TEST(parse_hardbreak_trailing_spaces) {
    /* Two trailing spaces before newline = hard break (plan §4.1).
     * "Hello  \nWorld" must produce TEXT("Hello"), HARDBREAK, TEXT("World"). */
    md_block_node_t *doc = md_parse("Hello  \nWorld");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    T_ASSERT(para->inlines != NULL, "paragraph should have inline content");

    /* Verify structure: TEXT, HARDBREAK, TEXT */
    md_inline_node_t *first = para->inlines;
    T_ASSERT(first->type == MD_INLINE_TEXT, "first inline should be TEXT, got %d", first->type);
    T_ASSERT(first->text != NULL && strcmp(first->text, "Hello") == 0,
             "first TEXT should be 'Hello', got '%s'", first->text ? first->text : "(null)");

    md_inline_node_t *second = first->next;
    T_ASSERT(second != NULL, "should have a second inline node");
    T_ASSERT(second->type == MD_INLINE_HARDBREAK,
             "second inline should be HARDBREAK, got %d", second->type);

    md_inline_node_t *third = second->next;
    T_ASSERT(third != NULL, "should have a third inline node");
    T_ASSERT(third->type == MD_INLINE_TEXT, "third inline should be TEXT, got %d", third->type);
    T_ASSERT(third->text != NULL && strcmp(third->text, "World") == 0,
             "third TEXT should be 'World', got '%s'", third->text ? third->text : "(null)");
    md_block_destroy(doc);
}

TEST(parse_no_hardbreak_one_space) {
    /* One trailing space before newline does NOT produce a hard break.
     * "Hello \nWorld" should merge into a single paragraph with no HARDBREAK. */
    md_block_node_t *doc = md_parse("Hello \nWorld");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");
    T_ASSERT(para->inlines != NULL, "paragraph should have inline content");

    /* Verify no HARDBREAK node exists */
    int found_hardbreak = 0;
    for (md_inline_node_t *inl = para->inlines; inl; inl = inl->next) {
        if (inl->type == MD_INLINE_HARDBREAK) found_hardbreak = 1;
    }
    T_ASSERT(!found_hardbreak,
             "single trailing space should NOT produce HARDBREAK");
    md_block_destroy(doc);
}

TEST(parse_hardbreak_many_spaces) {
    /* Five trailing spaces before newline should also produce a hard break. */
    md_block_node_t *doc = md_parse("Start     \nEnd");
    md_block_node_t *para = doc->children;
    T_ASSERT(para != NULL && para->type == MD_BLOCK_PARAGRAPH, "should be paragraph");

    int found_hardbreak = 0;
    for (md_inline_node_t *inl = para->inlines; inl; inl = inl->next) {
        if (inl->type == MD_INLINE_HARDBREAK) found_hardbreak = 1;
    }
    T_ASSERT(found_hardbreak, "5 trailing spaces should produce HARDBREAK");
    md_block_destroy(doc);
}

/* ================================================================
 * 14. PHASE 6 EDGE CASES
 * ================================================================ */

TEST(parse_single_char) {
    /* parse("X") returns DOCUMENT with one paragraph containing "X" */
    md_block_node_t *doc = md_parse("X");
    T_ASSERT(doc != NULL, "md_parse should not return NULL");
    T_ASSERT(doc->type == MD_BLOCK_DOCUMENT, "root should be DOCUMENT");
    T_ASSERT(doc->children != NULL, "should have a child");
    T_ASSERT(doc->children->type == MD_BLOCK_PARAGRAPH, "child should be paragraph");
    T_ASSERT(doc->children->inlines != NULL, "paragraph should have inline content");
    T_ASSERT(doc->children->inlines->text != NULL, "inline should have text");
    T_ASSERT(strcmp(doc->children->inlines->text, "X") == 0,
             "inline text should be 'X', got '%s'", doc->children->inlines->text);
    md_block_destroy(doc);
}

TEST(parse_extremely_long_line) {
    /* Parse a 10,000 character line — no crash, valid AST. */
    char *long_line = malloc(10001);
    T_ASSERT(long_line != NULL, "malloc failed");
    for (int i = 0; i < 10000; i++) long_line[i] = 'A' + (char)(i % 26);
    long_line[10000] = '\0';
    md_block_node_t *doc = md_parse(long_line);
    T_ASSERT(doc != NULL, "must not return NULL");
    T_ASSERT(doc->type == MD_BLOCK_DOCUMENT, "root should be DOCUMENT");
    T_ASSERT(doc->children != NULL, "should have a child");
    T_ASSERT(doc->children->type == MD_BLOCK_PARAGRAPH, "child should be paragraph");
    md_block_destroy(doc);
    free(long_line);
}

TEST(parse_binary_input) {
    /* parse("\x01\x02\x03\xff\xfe") — no crash. NUL bytes terminate strings
     * so we test without \x00, which is the C string terminator. */
    const char input[] = {'\x01', '\x02', '\x03', '\x7f', '\xfe', '\0'};
    md_block_node_t *doc = md_parse(input);
    T_ASSERT(doc != NULL, "must not return NULL on binary input");
    T_ASSERT(doc->type == MD_BLOCK_DOCUMENT, "root should be DOCUMENT");
    md_block_destroy(doc);
}

TEST(parse_whitespace_only_v2) {
    /* parse("   \n  \n   ") returns DOCUMENT with no meaningful content blocks. */
    md_block_node_t *doc = md_parse("   \n  \n   ");
    T_ASSERT(doc != NULL, "md_parse should not return NULL");
    T_ASSERT(doc->type == MD_BLOCK_DOCUMENT, "root should be DOCUMENT");
    /* Whitespace-only should produce no children */
    T_ASSERT(doc->children == NULL,
             "whitespace-only input should produce no children");
    md_block_destroy(doc);
}

/* ================================================================
 * 15. TABLE EDGE CASES
 * ================================================================ */

TEST(parse_table_mismatched_columns) {
    /* Plan §4.2: extra cells discarded, missing cells treated as empty. */
    const char *input =
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 | 3 |\n"
        "| x |\n";
    md_block_node_t *doc = md_parse(input);
    T_ASSERT(doc != NULL, "must not return NULL");
    md_block_node_t *table = doc->children;
    T_ASSERT(table != NULL && table->type == MD_BLOCK_TABLE, "should be a table");
    md_block_destroy(doc);
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(void) {
    printf("test_md_parse\n");
    printf("=============\n\n");

    printf("Empty / minimal input:\n");
    RUN(parse_empty_string);
    RUN(parse_whitespace_only);
    RUN(parse_single_word);

    printf("\nHeadings:\n");
    RUN(parse_h1);
    RUN(parse_h2);
    RUN(parse_h3);
    RUN(parse_h4);
    RUN(parse_h5_treated_as_h4);

    printf("\nHorizontal rules:\n");
    RUN(parse_hrule_dashes);
    RUN(parse_hrule_stars);
    RUN(parse_hrule_underscores);
    RUN(parse_hrule_many_chars);

    printf("\nParagraphs:\n");
    RUN(parse_single_paragraph);
    RUN(parse_two_paragraphs);
    RUN(parse_multiline_paragraph);

    printf("\nCode fences:\n");
    RUN(parse_code_fence_basic);
    RUN(parse_code_fence_with_language);
    RUN(parse_code_fence_no_inline_parse);
    RUN(parse_code_fence_unclosed);

    printf("\nTables:\n");
    RUN(parse_simple_table);
    RUN(parse_table_alignment);
    RUN(parse_table_header_row_flagged);

    printf("\nLists:\n");
    RUN(parse_unordered_list);
    RUN(parse_ordered_list);
    RUN(parse_nested_list);
    RUN(parse_ordered_list_start_number);

    printf("\nBlockquotes:\n");
    RUN(parse_blockquote);
    RUN(parse_nested_blockquote);

    printf("\nInline parsing:\n");
    RUN(parse_inline_bold);
    RUN(parse_inline_italic);
    RUN(parse_inline_bold_italic);
    RUN(parse_inline_code);
    RUN(parse_inline_link);
    RUN(parse_inline_code_no_inner_parse);

    printf("\nDocument structure:\n");
    RUN(parse_heading_then_paragraph);
    RUN(parse_heading_hrule_paragraph);
    RUN(parse_complex_document);

    printf("\nEdge cases:\n");
    RUN(parse_null_input);
    RUN(parse_binary_garbage);
    RUN(parse_very_long_line);
    RUN(parse_many_blank_lines);
    RUN(parse_heading_no_space);
    RUN(parse_mixed_list_markers);

    printf("\nInline nesting:\n");
    RUN(parse_bold_inside_italic);
    RUN(parse_link_with_bold_text);

    printf("\nSoft / hard breaks:\n");
    RUN(parse_softbreak);
    RUN(parse_hardbreak_trailing_spaces);
    RUN(parse_no_hardbreak_one_space);
    RUN(parse_hardbreak_many_spaces);

    printf("\nTable edge cases:\n");
    RUN(parse_table_mismatched_columns);

    printf("\nPhase 6 edge cases:\n");
    RUN(parse_single_char);
    RUN(parse_extremely_long_line);
    RUN(parse_binary_input);
    RUN(parse_whitespace_only_v2);

    printf("\n=============\n");
    printf("%d passed, %d failed, %d total\n", g_pass, g_fail, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}
