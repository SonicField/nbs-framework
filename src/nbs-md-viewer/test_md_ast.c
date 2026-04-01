/*
 * test_md_ast.c — Test suite for AST node allocation, tree construction,
 *                 and recursive deallocation.
 *
 * Key invariant tested:
 *   After md_block_destroy(), ZERO bytes of AST memory remain allocated.
 *   Falsifier: run under Valgrind or ASan — any leak report falsifies.
 *
 * Build:
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -I../nbs-common -o test_md_ast test_md_ast.c md_ast.c \
 *       && ./test_md_ast
 *
 * ASan:
 *   clang -fsanitize=address,undefined -g -O1 \
 *       -I../nbs-common -o test_md_ast test_md_ast.c md_ast.c \
 *       && ./test_md_ast
 */

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

/* ================================================================
 * 1. BLOCK NODE ALLOCATION
 *
 * Each block type must be allocatable and have the correct type tag.
 * All other fields must be zero/NULL.
 * ================================================================ */

TEST(block_node_new_returns_non_null) {
    md_block_node_t *n = md_block_create(MD_BLOCK_DOCUMENT);
    T_ASSERT(n != NULL, "md_block_create returned NULL");
    T_ASSERT(n->type == MD_BLOCK_DOCUMENT, "type should be DOCUMENT, got %d", n->type);
    T_ASSERT(n->children == NULL, "children should be NULL on fresh node");
    T_ASSERT(n->next == NULL, "next should be NULL on fresh node");
    T_ASSERT(n->inlines == NULL, "inlines should be NULL on fresh node");
    T_ASSERT(n->language == NULL, "language should be NULL on fresh node");
    T_ASSERT(n->raw == NULL, "raw should be NULL on fresh node");
    T_ASSERT(n->col_align == NULL, "col_align should be NULL on fresh node");
    T_ASSERT(n->level == 0, "level should be 0 on fresh node");
    T_ASSERT(n->ordered == 0, "ordered should be 0 on fresh node");
    T_ASSERT(n->start == 0, "start should be 0 on fresh node");
    T_ASSERT(n->is_header == 0, "is_header should be 0 on fresh node");
    T_ASSERT(n->col_count == 0, "col_count should be 0 on fresh node");
    md_block_destroy(n);
}

TEST(block_node_all_types_allocatable) {
    /* Every block type defined in the enum must be allocatable.
     * Falsifier: add a new block type to the enum without handling
     * it in md_block_create — this test still passes, but ASan
     * would catch uninitialised reads. */
    md_block_type_t types[] = {
        MD_BLOCK_DOCUMENT, MD_BLOCK_PARAGRAPH, MD_BLOCK_HEADING,
        MD_BLOCK_CODE_FENCE, MD_BLOCK_HRULE, MD_BLOCK_LIST,
        MD_BLOCK_LIST_ITEM, MD_BLOCK_TABLE, MD_BLOCK_TABLE_ROW,
        MD_BLOCK_TABLE_CELL, MD_BLOCK_BLOCKQUOTE
    };
    int count = (int)(sizeof(types) / sizeof(types[0]));
    T_ASSERT(count == 11, "expected 11 block types, got %d", count);

    for (int i = 0; i < count; i++) {
        md_block_node_t *n = md_block_create(types[i]);
        T_ASSERT(n != NULL, "md_block_create(%d) returned NULL", types[i]);
        T_ASSERT(n->type == types[i], "type mismatch for enum %d", types[i]);
        md_block_destroy(n);
    }
}

/* ================================================================
 * 2. INLINE NODE ALLOCATION
 * ================================================================ */

TEST(inline_node_new_returns_non_null) {
    md_inline_node_t *n = md_inline_create(MD_INLINE_TEXT);
    T_ASSERT(n != NULL, "md_inline_create returned NULL");
    T_ASSERT(n->type == MD_INLINE_TEXT, "type should be TEXT, got %d", n->type);
    T_ASSERT(n->text == NULL, "text should be NULL on fresh node");
    T_ASSERT(n->url == NULL, "url should be NULL on fresh node");
    T_ASSERT(n->children == NULL, "children should be NULL on fresh node");
    T_ASSERT(n->next == NULL, "next should be NULL on fresh node");
    /* Can't destroy inline without a block parent — free manually */
    free(n);
}

TEST(inline_node_all_types_allocatable) {
    md_inline_type_t types[] = {
        MD_INLINE_TEXT, MD_INLINE_BOLD, MD_INLINE_ITALIC,
        MD_INLINE_BOLD_ITALIC, MD_INLINE_CODE, MD_INLINE_LINK,
        MD_INLINE_SOFTBREAK, MD_INLINE_HARDBREAK
    };
    int count = (int)(sizeof(types) / sizeof(types[0]));
    T_ASSERT(count == 8, "expected 8 inline types, got %d", count);

    for (int i = 0; i < count; i++) {
        md_inline_node_t *n = md_inline_create(types[i]);
        T_ASSERT(n != NULL, "md_inline_create(%d) returned NULL", types[i]);
        T_ASSERT(n->type == types[i], "type mismatch for enum %d", types[i]);
        free(n);
    }
}

/* ================================================================
 * 3. TREE CONSTRUCTION — BLOCK CHILDREN
 *
 * md_block_add_child appends to the children linked list.
 * Order must be preserved. Parent must not be modified beyond children.
 * ================================================================ */

TEST(block_add_child_single) {
    md_block_node_t *doc = md_block_create(MD_BLOCK_DOCUMENT);
    md_block_node_t *para = md_block_create(MD_BLOCK_PARAGRAPH);
    md_block_add_child(doc, para);

    T_ASSERT(doc->children == para, "first child should be para");
    T_ASSERT(para->next == NULL, "single child next should be NULL");
    md_block_destroy(doc);
}

TEST(block_add_child_preserves_order) {
    md_block_node_t *doc = md_block_create(MD_BLOCK_DOCUMENT);
    md_block_node_t *h1 = md_block_create(MD_BLOCK_HEADING);
    md_block_node_t *p1 = md_block_create(MD_BLOCK_PARAGRAPH);
    md_block_node_t *hr = md_block_create(MD_BLOCK_HRULE);

    md_block_add_child(doc, h1);
    md_block_add_child(doc, p1);
    md_block_add_child(doc, hr);

    T_ASSERT(doc->children == h1, "first child should be h1");
    T_ASSERT(h1->next == p1, "second child should be p1");
    T_ASSERT(p1->next == hr, "third child should be hr");
    T_ASSERT(hr->next == NULL, "last child next should be NULL");
    md_block_destroy(doc);
}

/* ================================================================
 * 4. TREE CONSTRUCTION — INLINE CHILDREN
 * ================================================================ */

TEST(block_add_inline_single) {
    md_block_node_t *para = md_block_create(MD_BLOCK_PARAGRAPH);
    md_inline_node_t *text = md_inline_create(MD_INLINE_TEXT);
    text->text = strdup("Hello");
    md_block_add_inline(para, text);

    T_ASSERT(para->inlines == text, "paragraph inlines should point to text");
    T_ASSERT(text->next == NULL, "single inline next should be NULL");
    md_block_destroy(para);
}

TEST(inline_add_child_nested) {
    /* Build: bold -> (text "Hello") — tests inline nesting.
     * Destroy via parent block must free both. */
    md_block_node_t *para = md_block_create(MD_BLOCK_PARAGRAPH);
    md_inline_node_t *bold = md_inline_create(MD_INLINE_BOLD);
    md_inline_node_t *text = md_inline_create(MD_INLINE_TEXT);
    text->text = strdup("Hello");

    md_inline_add_child(bold, text);
    md_block_add_inline(para, bold);

    T_ASSERT(para->inlines == bold, "paragraph inlines should point to bold");
    T_ASSERT(bold->children == text, "bold children should point to text");
    T_ASSERT(strcmp(text->text, "Hello") == 0, "text content mismatch");
    md_block_destroy(para);
}

/* ================================================================
 * 5. DEEP TREE DESTRUCTION — MEMORY INVARIANT
 *
 * Build a realistic AST tree and destroy it. Under ASan/Valgrind,
 * any leak falsifies the memory invariant (plan §3.5).
 * ================================================================ */

TEST(destroy_complex_tree_no_leak) {
    /* Build: Document -> Heading(1) + Paragraph(inline text + bold(text) + link) + HRule */
    md_block_node_t *doc = md_block_create(MD_BLOCK_DOCUMENT);

    /* Heading */
    md_block_node_t *h1 = md_block_create(MD_BLOCK_HEADING);
    h1->level = 1;
    md_inline_node_t *h1_text = md_inline_create(MD_INLINE_TEXT);
    h1_text->text = strdup("Title");
    md_block_add_inline(h1, h1_text);
    md_block_add_child(doc, h1);

    /* Paragraph with mixed inlines */
    md_block_node_t *para = md_block_create(MD_BLOCK_PARAGRAPH);

    md_inline_node_t *t1 = md_inline_create(MD_INLINE_TEXT);
    t1->text = strdup("Before ");
    md_block_add_inline(para, t1);

    md_inline_node_t *bold = md_inline_create(MD_INLINE_BOLD);
    md_inline_node_t *bt = md_inline_create(MD_INLINE_TEXT);
    bt->text = strdup("bold");
    md_inline_add_child(bold, bt);
    md_block_add_inline(para, bold);

    md_inline_node_t *t2 = md_inline_create(MD_INLINE_TEXT);
    t2->text = strdup(" and ");
    md_block_add_inline(para, t2);

    md_inline_node_t *link = md_inline_create(MD_INLINE_LINK);
    link->url = strdup("https://example.com");
    md_inline_node_t *lt = md_inline_create(MD_INLINE_TEXT);
    lt->text = strdup("link text");
    md_inline_add_child(link, lt);
    md_block_add_inline(para, link);

    md_block_add_child(doc, para);

    /* HRule */
    md_block_node_t *hr = md_block_create(MD_BLOCK_HRULE);
    md_block_add_child(doc, hr);

    /* Destroy everything — ASan will catch any leak */
    md_block_destroy(doc);
}

TEST(destroy_code_fence_with_strings) {
    /* Code fence has owned strings (language, raw). Destroy must free them. */
    md_block_node_t *doc = md_block_create(MD_BLOCK_DOCUMENT);
    md_block_node_t *fence = md_block_create(MD_BLOCK_CODE_FENCE);
    fence->language = strdup("c");
    fence->raw = strdup("int main(void) { return 0; }");
    md_block_add_child(doc, fence);
    md_block_destroy(doc);
}

TEST(destroy_table_with_alignments) {
    /* Table has a malloc'd col_align array. Destroy must free it. */
    md_block_node_t *doc = md_block_create(MD_BLOCK_DOCUMENT);
    md_block_node_t *table = md_block_create(MD_BLOCK_TABLE);
    table->col_count = 3;
    table->col_align = malloc(3 * sizeof(md_align_t));
    T_ASSERT(table->col_align != NULL, "malloc failed for col_align");
    table->col_align[0] = MD_ALIGN_LEFT;
    table->col_align[1] = MD_ALIGN_CENTRE;
    table->col_align[2] = MD_ALIGN_RIGHT;

    /* Add a header row with cells */
    md_block_node_t *row = md_block_create(MD_BLOCK_TABLE_ROW);
    row->is_header = 1;
    for (int i = 0; i < 3; i++) {
        md_block_node_t *cell = md_block_create(MD_BLOCK_TABLE_CELL);
        md_inline_node_t *ct = md_inline_create(MD_INLINE_TEXT);
        ct->text = strdup("cell");
        md_block_add_inline(cell, ct);
        md_block_add_child(row, cell);
    }
    md_block_add_child(table, row);
    md_block_add_child(doc, table);
    md_block_destroy(doc);
}

TEST(destroy_nested_list) {
    /* Nested list: List -> ListItem -> List -> ListItem(text) */
    md_block_node_t *doc = md_block_create(MD_BLOCK_DOCUMENT);

    md_block_node_t *list = md_block_create(MD_BLOCK_LIST);
    md_block_node_t *item1 = md_block_create(MD_BLOCK_LIST_ITEM);
    md_inline_node_t *t1 = md_inline_create(MD_INLINE_TEXT);
    t1->text = strdup("Item 1");
    md_block_add_inline(item1, t1);

    /* Nested sublist inside item1 */
    md_block_node_t *sublist = md_block_create(MD_BLOCK_LIST);
    md_block_node_t *subitem = md_block_create(MD_BLOCK_LIST_ITEM);
    md_inline_node_t *st = md_inline_create(MD_INLINE_TEXT);
    st->text = strdup("Sub-item");
    md_block_add_inline(subitem, st);
    md_block_add_child(sublist, subitem);
    md_block_add_child(item1, sublist);

    md_block_add_child(list, item1);
    md_block_add_child(doc, list);
    md_block_destroy(doc);
}

TEST(destroy_null_is_safe) {
    /* md_block_destroy(NULL) must not crash. */
    md_block_destroy(NULL);
}

TEST(destroy_blockquote_with_nested_blocks) {
    /* Blockquote containing a paragraph and a nested blockquote. */
    md_block_node_t *doc = md_block_create(MD_BLOCK_DOCUMENT);
    md_block_node_t *bq = md_block_create(MD_BLOCK_BLOCKQUOTE);

    md_block_node_t *p1 = md_block_create(MD_BLOCK_PARAGRAPH);
    md_inline_node_t *t1 = md_inline_create(MD_INLINE_TEXT);
    t1->text = strdup("Quoted text");
    md_block_add_inline(p1, t1);
    md_block_add_child(bq, p1);

    md_block_node_t *inner_bq = md_block_create(MD_BLOCK_BLOCKQUOTE);
    md_block_node_t *p2 = md_block_create(MD_BLOCK_PARAGRAPH);
    md_inline_node_t *t2 = md_inline_create(MD_INLINE_TEXT);
    t2->text = strdup("Nested quote");
    md_block_add_inline(p2, t2);
    md_block_add_child(inner_bq, p2);
    md_block_add_child(bq, inner_bq);

    md_block_add_child(doc, bq);
    md_block_destroy(doc);
}

/* ================================================================
 * 6. INLINE SIBLING ORDER
 * ================================================================ */

TEST(inline_siblings_preserve_order) {
    /* Multiple inlines added to a block must preserve insertion order.
     * This is critical for paragraph rendering — the words must appear
     * in the order they were parsed. */
    md_block_node_t *para = md_block_create(MD_BLOCK_PARAGRAPH);

    const char *words[] = {"Hello", " ", "world", "!"};
    for (int i = 0; i < 4; i++) {
        md_inline_node_t *t = md_inline_create(MD_INLINE_TEXT);
        t->text = strdup(words[i]);
        md_block_add_inline(para, t);
    }

    md_inline_node_t *cur = para->inlines;
    for (int i = 0; i < 4; i++) {
        T_ASSERT(cur != NULL, "inline %d is NULL", i);
        T_ASSERT(cur->text != NULL, "inline %d text is NULL", i);
        T_ASSERT(strcmp(cur->text, words[i]) == 0,
                 "inline %d: expected \"%s\", got \"%s\"", i, words[i], cur->text);
        cur = cur->next;
    }
    T_ASSERT(cur == NULL, "should be exactly 4 inlines, but more exist");

    md_block_destroy(para);
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(void) {
    printf("test_md_ast\n");
    printf("===========\n\n");

    printf("Block node allocation:\n");
    RUN(block_node_new_returns_non_null);
    RUN(block_node_all_types_allocatable);

    printf("\nInline node allocation:\n");
    RUN(inline_node_new_returns_non_null);
    RUN(inline_node_all_types_allocatable);

    printf("\nTree construction — blocks:\n");
    RUN(block_add_child_single);
    RUN(block_add_child_preserves_order);

    printf("\nTree construction — inlines:\n");
    RUN(block_add_inline_single);
    RUN(inline_add_child_nested);

    printf("\nDestruction and memory invariant:\n");
    RUN(destroy_complex_tree_no_leak);
    RUN(destroy_code_fence_with_strings);
    RUN(destroy_table_with_alignments);
    RUN(destroy_nested_list);
    RUN(destroy_null_is_safe);
    RUN(destroy_blockquote_with_nested_blocks);

    printf("\nInline ordering:\n");
    RUN(inline_siblings_preserve_order);

    printf("\n===========\n");
    printf("%d passed, %d failed, %d total\n", g_pass, g_fail, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}
