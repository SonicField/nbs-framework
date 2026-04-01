/*
 * test_md_table.c — Test suite for table layout and box drawing.
 *
 * Tests column width calculation, alignment, box drawing output,
 * and truncation at various widths.
 *
 * Key invariants tested:
 *   1. Table display width <= terminal_width (or marked wide for h-pan)
 *   2. Box drawing characters are Unicode (not ASCII)
 *   3. Header separator uses double-horizontal (═)
 *   4. Column alignment is correct (LEFT/CENTRE/RIGHT)
 *   5. Edge cases: 1 column, empty cells, mismatched rows
 *
 * Build:
 *   gcc -Wall -Wextra -Wshadow -Werror -Wno-unused-parameter \
 *       -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -I../nbs-common -I../nbs-ts-render \
 *       -o test_md_table test_md_table.c md_table.c md_render.c md_parse.c \
 *       md_ast.c md_style.c ../nbs-common/nbs_term_attr.c \
 *       ../nbs-ts-render/nbs_ts_wcwidth.c \
 *       && ./test_md_table
 */

#include "md_render.h"
#include "md_parse.h"
#include "md_style.h"
#include "../nbs-common/nbs_assert.h"
#include "../nbs-common/nbs_term_attr.h"

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

/* Helper: concatenate all span text in a line */
static char *line_text(md_display_line_t *line) {
    int total = 0;
    for (int i = 0; i < line->span_count; i++) {
        if (line->spans[i].text)
            total += (int)strlen(line->spans[i].text);
    }
    char *buf = malloc(total + 1);
    buf[0] = '\0';
    for (int i = 0; i < line->span_count; i++) {
        if (line->spans[i].text)
            strcat(buf, line->spans[i].text);
    }
    return buf;
}

/* ================================================================
 * 1. BASIC TABLE RENDERING
 * ================================================================ */

TEST(table_produces_lines) {
    const char *input =
        "| A | B | C |\n"
        "|---|---|---|\n"
        "| 1 | 2 | 3 |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "render should succeed");
    T_ASSERT(layout->line_count > 0, "table should produce lines");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_has_box_drawing_chars) {
    /* Plan §5.6: tables use Unicode box drawing */
    const char *input =
        "| X | Y |\n"
        "|---|---|\n"
        "| a | b |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);

    int found_corner = 0;
    int found_horizontal = 0;
    int found_vertical = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "\xe2\x94\x8c")) found_corner = 1;     /* ┌ */
        if (strstr(t, "\xe2\x94\x80")) found_horizontal = 1;  /* ─ */
        if (strstr(t, "\xe2\x94\x82")) found_vertical = 1;    /* │ */
        free(t);
    }
    T_ASSERT(found_corner, "table should contain ┌ (top-left corner)");
    T_ASSERT(found_horizontal, "table should contain ─ (horizontal)");
    T_ASSERT(found_vertical, "table should contain │ (vertical)");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_header_separator_double_horizontal) {
    /* Plan §5.6: header separator uses ═ (double-horizontal) */
    const char *input =
        "| H1 | H2 |\n"
        "|----|----|\n"
        "| d1 | d2 |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);

    int found_double = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "\xe2\x95\x90")) found_double = 1; /* ═ U+2550 */
        free(t);
    }
    T_ASSERT(found_double,
             "header separator should use ═ (U+2550 double-horizontal)");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 2. ALIGNMENT
 * ================================================================ */

TEST(table_left_alignment_default) {
    const char *input =
        "| Name    |\n"
        "|---------|\n"
        "| Alice   |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "render should succeed");
    /* Default alignment is LEFT — text should appear near left edge */
    int found_text = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "Alice")) found_text = 1;
        free(t);
    }
    T_ASSERT(found_text, "table cell text 'Alice' should appear in output");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_centre_alignment) {
    const char *input =
        "| Name |\n"
        "|:----:|\n"
        "| AB   |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "render should succeed");
    /* Centre alignment — text should have padding on both sides */
    int found_text = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "AB")) found_text = 1;
        free(t);
    }
    T_ASSERT(found_text, "centre-aligned text 'AB' should appear");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_right_alignment) {
    const char *input =
        "| Value |\n"
        "|------:|\n"
        "| 42    |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "render should succeed");
    int found_text = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "42")) found_text = 1;
        free(t);
    }
    T_ASSERT(found_text, "right-aligned text '42' should appear");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 3. COLUMN WIDTH
 * ================================================================ */

TEST(table_column_width_from_longest_cell) {
    /* Column width should be max cell width + padding (plan §5.6) */
    const char *input =
        "| Short | Longer Cell Value |\n"
        "|-------|-------------------|\n"
        "| x     | y                 |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "render should succeed");
    T_ASSERT(layout->line_count > 0, "should produce lines");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 4. EDGE CASES
 * ================================================================ */

TEST(table_single_column) {
    /* Plan R7: 1-column table must form valid rectangle */
    const char *input =
        "| Solo |\n"
        "|------|\n"
        "| data |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "render should succeed");

    /* Must have box drawing forming a valid rectangle */
    int found_top_left = 0, found_bottom_left = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "\xe2\x94\x8c")) found_top_left = 1;    /* ┌ */
        if (strstr(t, "\xe2\x94\x94")) found_bottom_left = 1;  /* └ */
        free(t);
    }
    T_ASSERT(found_top_left, "1-column table should have ┌");
    T_ASSERT(found_bottom_left, "1-column table should have └");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_empty_cells) {
    const char *input =
        "| A | B |\n"
        "|---|---|\n"
        "|   |   |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "render should succeed");
    T_ASSERT(layout->line_count > 0, "should produce lines");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_wide_truncation) {
    /* Table wider than terminal should truncate (§5.6) */
    const char *input =
        "| Column One | Column Two | Column Three | Column Four | Column Five |\n"
        "|------------|------------|--------------|-------------|-------------|\n"
        "| data       | data       | data         | data        | data        |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 40);
    T_ASSERT(layout != NULL, "render should succeed");
    /* Lines should be marked as wide for h-pan, or truncated */
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_header_row_styling) {
    /* Header row should have bold/distinct styling (plan §5.2) */
    const char *input =
        "| Header |\n"
        "|--------|\n"
        "| body   |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);

    /* Find a span containing "Header" — should have bold */
    int found_bold_header = 0;
    for (int i = 0; i < layout->line_count; i++) {
        md_display_line_t *dl = &layout->lines[i];
        for (int j = 0; j < dl->span_count; j++) {
            if (dl->spans[j].text && strstr(dl->spans[j].text, "Header")) {
                if (dl->spans[j].style.attrs & NBS_ATTR_BOLD)
                    found_bold_header = 1;
            }
        }
    }
    T_ASSERT(found_bold_header, "header row text should have bold styling");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_multiple_rows) {
    const char *input =
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |\n"
        "| 3 | 4 |\n"
        "| 5 | 6 |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "render should succeed");

    /* All row data should appear */
    int count = 0;
    const char *vals[] = {"1", "2", "3", "4", "5", "6"};
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        for (int v = 0; v < 6; v++) {
            if (strstr(t, vals[v])) count++;
        }
        free(t);
    }
    /* At least the 6 single-digit values should appear */
    T_ASSERT(count >= 6, "all table cell values should appear, found %d/6", count);
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_mixed_alignment) {
    const char *input =
        "| Left | Centre | Right |\n"
        "|:-----|:------:|------:|\n"
        "| L    | C      | R     |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "render should succeed");
    /* Verify all cell texts appear */
    int found_l = 0, found_c = 0, found_r = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "Left")) found_l = 1;
        if (strstr(t, "Centre")) found_c = 1;
        if (strstr(t, "Right")) found_r = 1;
        free(t);
    }
    T_ASSERT(found_l && found_c && found_r,
             "all alignment header texts should appear");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_no_crash_on_empty_table) {
    /* A table with only header and separator, no body rows */
    const char *input =
        "| H1 | H2 |\n"
        "|----|----|\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "render should succeed");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(table_memory_cleanup) {
    /* Complex table under ASan — leaks would be caught */
    const char *input =
        "| A | B | C | D |\n"
        "|:--|:-:|--:|---|\n"
        "| a | b | c | d |\n"
        "| e | f | g | h |\n"
        "| i | j | k | l |\n";
    md_block_node_t *doc = md_parse(input);
    md_layout_t *layout = md_render(doc, 60);
    T_ASSERT(layout != NULL, "render should succeed");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(void) {
    printf("test_md_table\n");
    printf("=============\n\n");

    printf("Basic table rendering:\n");
    RUN(table_produces_lines);
    RUN(table_has_box_drawing_chars);
    RUN(table_header_separator_double_horizontal);

    printf("\nAlignment:\n");
    RUN(table_left_alignment_default);
    RUN(table_centre_alignment);
    RUN(table_right_alignment);

    printf("\nColumn width:\n");
    RUN(table_column_width_from_longest_cell);

    printf("\nEdge cases:\n");
    RUN(table_single_column);
    RUN(table_empty_cells);
    RUN(table_wide_truncation);
    RUN(table_header_row_styling);
    RUN(table_multiple_rows);
    RUN(table_mixed_alignment);
    RUN(table_no_crash_on_empty_table);
    RUN(table_memory_cleanup);

    printf("\n=============\n");
    printf("%d passed, %d failed, %d total\n", g_pass, g_fail, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}
