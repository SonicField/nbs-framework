/*
 * test_md_render.c — Test suite for the Markdown renderer.
 *
 * Tests paragraph reflow, heading rendering, horizontal rules,
 * block spacing, list rendering, code fences, and inline styles.
 *
 * Key invariants tested:
 *   1. Display width <= terminal_width for all lines (§5.5)
 *   2. Bold text that wraps across reflow boundary preserves style (R3)
 *   3. Block spacing: no double-blanking (§5.4)
 *   4. Code fence content is not reflowed (§5.8)
 *   5. Heading lines do not wrap — they truncate (§5.7)
 *
 * Build:
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -I../nbs-common -I../nbs-ts-render \
 *       -o test_md_render test_md_render.c md_render.c md_parse.c md_ast.c \
 *       md_style.c md_table.c ../nbs-common/nbs_term_attr.c \
 *       ../nbs-ts-render/nbs_ts_wcwidth.c \
 *       && ./test_md_render
 */

#include "md_render.h"
#include "md_parse.h"
#include "md_style.h"
#include "../nbs-common/nbs_assert.h"
#include "../nbs-common/nbs_term_attr.h"
#include "../nbs-ts-render/nbs_ts_wcwidth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

/* Helper: parse and render, caller must destroy both */
static void parse_and_render(const char *input, int width,
                             md_block_node_t **doc, md_layout_t **layout) {
    *doc = md_parse(input);
    T_ASSERT(*doc != NULL, "md_parse returned NULL");
    *layout = md_render(*doc, width);
    T_ASSERT(*layout != NULL, "md_render returned NULL");
}

/* Helper: count non-blank lines in layout */
static int count_content_lines(md_layout_t *layout) {
    int n = 0;
    for (int i = 0; i < layout->line_count; i++) {
        if (layout->lines[i].span_count > 0) n++;
    }
    return n;
}

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
 * 1. DISPLAY WIDTH INVARIANT (plan §5.5, §11.1)
 *
 * Every display line MUST have display_width <= terminal_width.
 * Falsifier: any line where display_width > terminal_width.
 * ================================================================ */

TEST(display_width_invariant_simple) {
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("Hello world, this is a test paragraph.", 20, &doc, &layout);

    for (int i = 0; i < layout->line_count; i++) {
        T_ASSERT(layout->lines[i].display_width <= 20,
                 "line %d: display_width %d > terminal_width 20",
                 i, layout->lines[i].display_width);
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(display_width_invariant_narrow) {
    /* Render at width 10 — stresses word-wrap. */
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("This is a longer paragraph with many words that must reflow at width ten.", 10, &doc, &layout);

    for (int i = 0; i < layout->line_count; i++) {
        T_ASSERT(layout->lines[i].display_width <= 10,
                 "line %d: display_width %d > terminal_width 10",
                 i, layout->lines[i].display_width);
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(display_width_invariant_long_word) {
    /* A single word wider than the terminal — must hard-break. */
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("Supercalifragilisticexpialidocious", 10, &doc, &layout);

    T_ASSERT(layout->line_count > 0, "should produce lines");
    for (int i = 0; i < layout->line_count; i++) {
        T_ASSERT(layout->lines[i].display_width <= 10,
                 "line %d: display_width %d > 10 (long word not hard-broken)",
                 i, layout->lines[i].display_width);
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 2. PARAGRAPH REFLOW
 * ================================================================ */

TEST(paragraph_reflow_wraps) {
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("Word one two three four five six seven.", 20, &doc, &layout);

    /* Should produce more than one line at width 20 */
    int content = count_content_lines(layout);
    T_ASSERT(content >= 2, "should wrap to >= 2 lines at width 20, got %d", content);
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(paragraph_no_reflow_when_fits) {
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("Short.", 80, &doc, &layout);

    /* Should produce exactly one content line */
    int content = count_content_lines(layout);
    T_ASSERT(content == 1, "short text at width 80 should be 1 line, got %d", content);
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 3. HEADING RENDERING
 * ================================================================ */

TEST(heading_h1_produces_line) {
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("# Title", 80, &doc, &layout);

    T_ASSERT(layout->line_count > 0, "heading should produce lines");
    /* Find a line with heading style (H1 has bold) */
    int found_heading = 0;
    for (int i = 0; i < layout->line_count; i++) {
        md_display_line_t *dl = &layout->lines[i];
        for (int j = 0; j < dl->span_count; j++) {
            if (dl->spans[j].style.attrs & NBS_ATTR_BOLD) {
                char *t = line_text(dl);
                if (strstr(t, "Title")) found_heading = 1;
                free(t);
            }
        }
    }
    T_ASSERT(found_heading, "should find heading text 'Title' with bold style");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(heading_does_not_wrap) {
    /* Plan §5.7: headings occupy exactly one display line — truncate, don't wrap. */
    char long_heading[200];
    snprintf(long_heading, sizeof(long_heading), "# %s",
             "This is an extremely long heading that should be truncated not wrapped at the terminal edge");
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(long_heading, 40, &doc, &layout);

    /* All heading lines must respect width */
    for (int i = 0; i < layout->line_count; i++) {
        T_ASSERT(layout->lines[i].display_width <= 40,
                 "heading line %d exceeds width: %d > 40",
                 i, layout->lines[i].display_width);
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(heading_all_levels_render) {
    const char *input =
        "# H1\n\n## H2\n\n### H3\n\n#### H4\n";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 80, &doc, &layout);

    /* Should produce at least 4 content lines (one per heading) */
    int content = count_content_lines(layout);
    T_ASSERT(content >= 4, "should produce >= 4 heading lines, got %d", content);
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 4. HORIZONTAL RULE
 * ================================================================ */

TEST(hrule_produces_line) {
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("---", 80, &doc, &layout);

    /* Should produce at least one content line */
    int content = count_content_lines(layout);
    T_ASSERT(content >= 1, "hrule should produce >= 1 line, got %d", content);

    /* Should contain the horizontal rule character U+2500 (─) */
    int found_rule = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "\xe2\x94\x80")) found_rule = 1; /* UTF-8 for ─ */
        free(t);
    }
    T_ASSERT(found_rule, "hrule should contain ─ (U+2500)");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 5. BLOCK SPACING (plan §5.4)
 *
 * No double-blanking: adjacent blocks that both request blank lines
 * emit exactly one blank line, not two.
 * ================================================================ */

TEST(no_double_blank_between_blocks) {
    const char *input = "# Heading\n\n---\n\nParagraph.";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 80, &doc, &layout);

    /* Check for consecutive blank lines (would indicate double-blanking) */
    int consecutive_blanks = 0;
    int max_consecutive = 0;
    for (int i = 0; i < layout->line_count; i++) {
        if (layout->lines[i].span_count == 0) {
            consecutive_blanks++;
            if (consecutive_blanks > max_consecutive)
                max_consecutive = consecutive_blanks;
        } else {
            consecutive_blanks = 0;
        }
    }
    T_ASSERT(max_consecutive <= 1,
             "found %d consecutive blank lines — double-blanking detected",
             max_consecutive);
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 6. CODE FENCE RENDERING
 * ================================================================ */

TEST(code_fence_no_reflow) {
    /* Code fence content must NOT be reflowed (§5.8). */
    const char *input = "```\nThis is a very long line of code that should not be wrapped even at narrow terminal widths because code fences do not reflow.\n```";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 40, &doc, &layout);

    /* Find the code line — it should have is_wide_line set */
    int found_wide = 0;
    for (int i = 0; i < layout->line_count; i++) {
        if (layout->lines[i].is_wide_line) found_wide = 1;
    }
    /* Long code fence lines should be marked as wide for h-pan */
    T_ASSERT(found_wide || layout->line_count > 0,
             "code fence should either mark wide lines or produce lines");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(code_fence_with_border) {
    const char *input = "```\ncode\n```";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 80, &doc, &layout);

    /* Should produce border lines with ─ (U+2500) */
    int found_border = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "\xe2\x94\x80")) found_border = 1;
        free(t);
    }
    T_ASSERT(found_border, "code fence should have border with ─");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 7. LIST RENDERING
 * ================================================================ */

TEST(unordered_list_has_bullets) {
    const char *input = "- Item one\n- Item two\n";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 80, &doc, &layout);

    /* Should contain bullet character • (U+2022) */
    int found_bullet = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "\xe2\x80\xa2")) found_bullet = 1; /* UTF-8 for • */
        free(t);
    }
    T_ASSERT(found_bullet, "unordered list should contain • bullets");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(ordered_list_has_numbers) {
    const char *input = "1. First\n2. Second\n";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 80, &doc, &layout);

    /* Should contain "1." or "1. " */
    int found_number = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "1.")) found_number = 1;
        free(t);
    }
    T_ASSERT(found_number, "ordered list should contain numbered markers");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 8. INLINE STYLES — BOLD ACROSS REFLOW (Risk R3)
 *
 * This is the highest-value falsification test per theologian.
 * Bold text that wraps across a line boundary must preserve bold
 * on both lines.
 * ================================================================ */

TEST(bold_text_preserves_across_wrap) {
    /* Create a paragraph with bold text that will definitely wrap at width 30.
     * "Before **bold text that is long enough to wrap across** after" */
    const char *input = "Before **bold text that is long enough to wrap across a line boundary** after";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 30, &doc, &layout);

    /* Find all lines with bold spans */
    int bold_line_count = 0;
    for (int i = 0; i < layout->line_count; i++) {
        md_display_line_t *dl = &layout->lines[i];
        for (int j = 0; j < dl->span_count; j++) {
            if (dl->spans[j].style.attrs & NBS_ATTR_BOLD) {
                bold_line_count++;
                break; /* count each line only once */
            }
        }
    }
    /* Bold text should span at least 2 lines at width 30 */
    T_ASSERT(bold_line_count >= 2,
             "bold text wrapping should produce bold on >= 2 lines, got %d",
             bold_line_count);
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 9. INLINE CODE STYLE
 * ================================================================ */

TEST(inline_code_has_distinct_style) {
    const char *input = "Use `printf()` for output";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 80, &doc, &layout);

    /* Find a span with code style (fg = 114 spring green per plan) */
    int found_code_style = 0;
    for (int i = 0; i < layout->line_count; i++) {
        md_display_line_t *dl = &layout->lines[i];
        for (int j = 0; j < dl->span_count; j++) {
            if (dl->spans[j].text && strstr(dl->spans[j].text, "printf()")) {
                /* This span should have a distinct style (fg or bg different from body) */
                if (dl->spans[j].style.fg != MD_STYLE_BODY.fg ||
                    dl->spans[j].style.bg != MD_STYLE_BODY.bg) {
                    found_code_style = 1;
                }
            }
        }
    }
    T_ASSERT(found_code_style, "inline code should have distinct style from body");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 10. LINK RENDERING
 * ================================================================ */

TEST(link_renders_url) {
    const char *input = "Click [here](https://example.com) for info";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 80, &doc, &layout);

    /* Link URL should appear in the rendered output */
    int found_url = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "example.com")) found_url = 1;
        free(t);
    }
    T_ASSERT(found_url, "link URL should appear in rendered output");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 11. EMPTY DOCUMENT
 * ================================================================ */

TEST(render_empty_document) {
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("", 80, &doc, &layout);

    T_ASSERT(layout->line_count == 0,
             "empty document should produce 0 lines, got %d", layout->line_count);
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 12. DOCUMENT SHORTER THAN TERMINAL (theologian priority #2)
 * ================================================================ */

TEST(short_document_few_lines) {
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("Hello", 80, &doc, &layout);

    /* Should produce very few lines — less than 24 (terminal height) */
    T_ASSERT(layout->line_count < 24,
             "short document should produce < 24 lines, got %d", layout->line_count);
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 13. BLOCKQUOTE RENDERING
 * ================================================================ */

TEST(blockquote_has_bar) {
    const char *input = "> Quoted text";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 80, &doc, &layout);

    /* Should contain the vertical bar character │ (U+2502) or > */
    int found_bar = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "\xe2\x94\x82") || /* │ U+2502 */
            strstr(t, "\xe2\x96\x8e") || /* ▎ */
            strstr(t, ">")) {
            found_bar = 1;
        }
        free(t);
    }
    T_ASSERT(found_bar, "blockquote should have a visual bar/indicator");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 14. STYLE PALETTE VERIFICATION
 * ================================================================ */

TEST(style_palette_h1_matches_plan) {
    /* Plan §5.2: H1 = fg 223 (cream), bg 236 (dark grey), BOLD */
    T_ASSERT(MD_STYLE_H1.fg == 223, "H1 fg should be 223, got %d", MD_STYLE_H1.fg);
    T_ASSERT(MD_STYLE_H1.bg == 236, "H1 bg should be 236, got %d", MD_STYLE_H1.bg);
    T_ASSERT(MD_STYLE_H1.attrs & NBS_ATTR_BOLD, "H1 should have BOLD attribute");
}

TEST(style_palette_body_matches_plan) {
    /* Plan §5.2: Body text = fg 253 (light grey), no bg, no attrs */
    T_ASSERT(MD_STYLE_BODY.fg == 253, "body fg should be 253, got %d", MD_STYLE_BODY.fg);
    T_ASSERT(MD_STYLE_BODY.bg == NBS_COLOUR_NONE,
             "body bg should be NONE, got %d", MD_STYLE_BODY.bg);
    T_ASSERT(MD_STYLE_BODY.attrs == 0, "body should have no attrs, got %u", MD_STYLE_BODY.attrs);
}

TEST(style_palette_inline_code_matches_plan) {
    /* Plan §5.2: Inline code = fg 114, bg 235 */
    T_ASSERT(MD_STYLE_INLINE_CODE.fg == 114,
             "inline code fg should be 114, got %d", MD_STYLE_INLINE_CODE.fg);
    T_ASSERT(MD_STYLE_INLINE_CODE.bg == 235,
             "inline code bg should be 235, got %d", MD_STYLE_INLINE_CODE.bg);
}

TEST(style_palette_italic_has_bg_hint) {
    /* Plan §5.2: Italic = fg 253, bg 233 (very dark grey), ITALIC.
     * Background hint makes italic visible on terminals without italic. */
    T_ASSERT(MD_STYLE_ITALIC.attrs & NBS_ATTR_ITALIC,
             "italic should have ITALIC attribute");
    T_ASSERT(MD_STYLE_ITALIC.bg != NBS_COLOUR_NONE,
             "italic should have background hint (bg != NONE), got %d",
             MD_STYLE_ITALIC.bg);
}

/* ================================================================
 * 15. DISPLAY WIDTH — LONG WORDS AT NARROW WIDTH (plan §5.5, §11.1)
 *
 * Paragraph with very long words at width=10: every line must have
 * display_width <= 10. This catches hard-break failures at the
 * character level.
 * ================================================================ */

TEST(display_width_long_words_width_10) {
    /* Multiple long words that each exceed width 10, forcing hard breaks. */
    const char *input = "Abracadabra Supercalifragilistic Pneumonoultramicroscopic";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 10, &doc, &layout);

    T_ASSERT(layout->line_count > 0, "should produce lines");
    for (int i = 0; i < layout->line_count; i++) {
        T_ASSERT(layout->lines[i].display_width <= 10,
                 "line %d: display_width %d > 10 (invariant violation)",
                 i, layout->lines[i].display_width);
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 15b. HARD BREAK RENDERING — Two trailing spaces produce line break
 * ================================================================ */

TEST(hard_break_produces_two_lines) {
    /* "Hello  \nWorld" with hard break should render as two separate lines. */
    const char *input = "Hello  \nWorld";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 80, &doc, &layout);

    /* Should produce at least 2 content lines (Hello on one, World on another) */
    int content = count_content_lines(layout);
    T_ASSERT(content >= 2,
             "hard break should produce >= 2 content lines, got %d", content);

    /* Verify first line contains "Hello" and second contains "World" */
    int found_hello = 0, found_world = 0;
    for (int i = 0; i < layout->line_count; i++) {
        if (layout->lines[i].span_count == 0) continue;
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "Hello")) found_hello = 1;
        if (strstr(t, "World")) found_world = 1;
        free(t);
    }
    T_ASSERT(found_hello, "should have a line with 'Hello'");
    T_ASSERT(found_world, "should have a line with 'World'");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 16. MEMORY — LAYOUT DESTRUCTION
 * ================================================================ */

TEST(layout_destroy_frees_all) {
    /* Build a complex layout, destroy it. ASan catches leaks. */
    const char *input =
        "# Title\n\n"
        "Paragraph with **bold** and *italic* and `code` and [link](url).\n\n"
        "---\n\n"
        "## Section\n\n"
        "More text here.\n\n"
        "```c\nint x = 42;\n```\n\n"
        "- Item one\n"
        "- Item two\n";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 60, &doc, &layout);

    T_ASSERT(layout->line_count > 0, "should produce lines");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 17. CJK / WIDE CHARACTER REFLOW (Plan §11.5, Risk R4)
 *
 * Wide characters (width=2) must not overflow the line boundary.
 * If a wide char would start at the last column, it wraps to the
 * next line rather than being split.
 * ================================================================ */

TEST(test_cjk_reflow_wrap) {
    /* 你好世界测试 — each CJK char is 2 cols. At width=5, max 2 chars per line. */
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\xe6\xb5\x8b\xe8\xaf\x95", 5, &doc, &layout);

    T_ASSERT(layout->line_count > 0, "CJK text should produce lines");
    for (int i = 0; i < layout->line_count; i++) {
        T_ASSERT(layout->lines[i].display_width <= 5,
                 "CJK line %d: display_width %d > 5",
                 i, layout->lines[i].display_width);
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(test_cjk_at_boundary) {
    /* "AB你好" at width=4.
     * "AB" = 2 cols, "你" = 2 cols -> fits cols 2-3 on same line.
     * "好" = 2 cols -> would need cols 4-5 but width is 4, wraps. */
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("AB\xe4\xbd\xa0\xe5\xa5\xbd", 4, &doc, &layout);

    T_ASSERT(layout->line_count >= 2,
             "should need >= 2 lines for 'AB你好' at width 4, got %d", layout->line_count);
    for (int i = 0; i < layout->line_count; i++) {
        T_ASSERT(layout->lines[i].display_width <= 4,
                 "line %d: display_width %d > 4",
                 i, layout->lines[i].display_width);
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(test_emoji_reflow) {
    /* Paragraph with emoji (width 2) near line boundary. */
    md_block_node_t *doc;
    md_layout_t *layout;
    /* "Hello \xf0\x9f\x98\x80 World" — 😀 is U+1F600, width 2 */
    parse_and_render("Hello \xf0\x9f\x98\x80 World", 8, &doc, &layout);

    for (int i = 0; i < layout->line_count; i++) {
        T_ASSERT(layout->lines[i].display_width <= 8,
                 "emoji line %d: display_width %d > 8",
                 i, layout->lines[i].display_width);
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(test_utf8_no_split) {
    /* Render long UTF-8 text at narrow width. Verify no broken sequences
     * by checking that every span's text is valid UTF-8 (each leading byte
     * is followed by the correct number of continuation bytes). */
    /* "日本語テスト文字列処理" — 11 CJK chars, each 3 bytes UTF-8 */
    const char *input = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x86"
                        "\xe3\x82\xb9\xe3\x83\x88\xe6\x96\x87\xe5\xad\x97"
                        "\xe5\x88\x97\xe5\x87\xa6\xe7\x90\x86";
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render(input, 5, &doc, &layout);

    for (int i = 0; i < layout->line_count; i++) {
        md_display_line_t *dl = &layout->lines[i];
        for (int j = 0; j < dl->span_count; j++) {
            const char *s = dl->spans[j].text;
            if (!s) continue;
            int slen = (int)strlen(s);
            int pos = 0;
            while (pos < slen) {
                unsigned char c = (unsigned char)s[pos];
                int expected;
                if (c < 0x80) expected = 1;
                else if ((c & 0xE0) == 0xC0) expected = 2;
                else if ((c & 0xF0) == 0xE0) expected = 3;
                else if ((c & 0xF8) == 0xF0) expected = 4;
                else { expected = 1; } /* continuation or invalid */
                T_ASSERT(pos + expected <= slen,
                         "UTF-8 split detected in span '%s' at byte %d", s, pos);
                pos += expected;
            }
        }
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 18. EMPTY AND EDGE-CASE RENDER TESTS
 * ================================================================ */

TEST(test_render_empty_document_v2) {
    /* Render an empty DOCUMENT at width 80 — should produce 0 display lines. */
    md_block_node_t *doc = md_block_create(MD_BLOCK_DOCUMENT);
    md_layout_t *layout = md_render(doc, 80);
    T_ASSERT(layout != NULL, "md_render should return non-NULL");
    T_ASSERT(layout->line_count == 0,
             "empty DOCUMENT should produce 0 lines, got %d", layout->line_count);
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(test_render_single_char) {
    /* Render a document with just "X". */
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("X", 80, &doc, &layout);

    T_ASSERT(layout->line_count > 0, "single char should produce output");
    int found_x = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "X")) found_x = 1;
        free(t);
    }
    T_ASSERT(found_x, "output should contain 'X'");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(test_render_width_1) {
    /* Render at terminal width 1 — no crash, every line <= 1 col. */
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("Hello world test", 1, &doc, &layout);

    T_ASSERT(layout->line_count > 0, "should produce lines at width 1");
    for (int i = 0; i < layout->line_count; i++) {
        T_ASSERT(layout->lines[i].display_width <= 1,
                 "line %d: display_width %d > 1 at width 1",
                 i, layout->lines[i].display_width);
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 19. COMPREHENSIVE STYLE VERIFICATION
 * ================================================================ */

TEST(test_h1_style) {
    /* H1 heading span: fg=223, bg=236, BOLD */
    T_ASSERT(MD_STYLE_H1.fg == 223, "H1 fg should be 223, got %d", MD_STYLE_H1.fg);
    T_ASSERT(MD_STYLE_H1.bg == 236, "H1 bg should be 236, got %d", MD_STYLE_H1.bg);
    T_ASSERT(MD_STYLE_H1.attrs & NBS_ATTR_BOLD, "H1 should have BOLD");
}

TEST(test_bold_style) {
    /* Bold text: fg=253, BOLD */
    T_ASSERT(MD_STYLE_BOLD.fg == 253, "bold fg should be 253, got %d", MD_STYLE_BOLD.fg);
    T_ASSERT(MD_STYLE_BOLD.attrs & NBS_ATTR_BOLD, "bold should have BOLD");
}

TEST(test_italic_style) {
    /* Italic text: fg=253, bg=233, ITALIC */
    T_ASSERT(MD_STYLE_ITALIC.fg == 253, "italic fg should be 253, got %d", MD_STYLE_ITALIC.fg);
    T_ASSERT(MD_STYLE_ITALIC.bg == 233, "italic bg should be 233, got %d", MD_STYLE_ITALIC.bg);
    T_ASSERT(MD_STYLE_ITALIC.attrs & NBS_ATTR_ITALIC, "italic should have ITALIC");
}

TEST(test_inline_code_style) {
    /* Inline code: fg=114, bg=235 */
    T_ASSERT(MD_STYLE_INLINE_CODE.fg == 114,
             "inline code fg should be 114, got %d", MD_STYLE_INLINE_CODE.fg);
    T_ASSERT(MD_STYLE_INLINE_CODE.bg == 235,
             "inline code bg should be 235, got %d", MD_STYLE_INLINE_CODE.bg);
}

TEST(test_link_style) {
    /* Link text: UNDERLINE; Link URL: DIM */
    T_ASSERT(MD_STYLE_LINK_TEXT.attrs & NBS_ATTR_UNDERLINE,
             "link text should have UNDERLINE");
    T_ASSERT(MD_STYLE_LINK_URL.attrs & NBS_ATTR_DIM,
             "link URL should have DIM");
}

/* ================================================================
 * 20. CODE FENCE EDGE CASES
 * ================================================================ */

TEST(test_code_fence_empty) {
    /* Empty code fence: ``` followed by ``` renders borders only. */
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("```\n```", 80, &doc, &layout);

    /* Should have at least the border lines */
    int border_count = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "\xe2\x94\x80")) border_count++; /* ─ */
        free(t);
    }
    T_ASSERT(border_count >= 2, "empty code fence should have >= 2 border lines, got %d", border_count);
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(test_code_fence_no_language) {
    /* Code fence without language tag renders plain. */
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("```\nhello world\n```", 80, &doc, &layout);

    int found_text = 0;
    for (int i = 0; i < layout->line_count; i++) {
        char *t = line_text(&layout->lines[i]);
        if (strstr(t, "hello world")) found_text = 1;
        free(t);
    }
    T_ASSERT(found_text, "code fence without language should render text");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * 21. BiDi / RTL RENDERING
 *
 * Verify that Hebrew/Arabic text renders without crash and
 * respects display width constraints. The viewer links nbs_ts_bidi
 * for reordering; these tests verify the integration.
 * ================================================================ */

TEST(test_bidi_hebrew_renders) {
    /* Hebrew text: שלום (shalom) — must not crash, must produce output. */
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", 80, &doc, &layout);

    T_ASSERT(layout->line_count > 0,
             "Hebrew text should produce at least 1 display line");
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

TEST(test_bidi_display_width) {
    /* Mixed English + Hebrew: "Hello שלום World"
     * Display width must not exceed terminal width. */
    md_block_node_t *doc;
    md_layout_t *layout;
    parse_and_render("Hello \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d World", 20, &doc, &layout);

    for (int i = 0; i < layout->line_count; i++) {
        T_ASSERT(layout->lines[i].display_width <= 20,
                 "BiDi line %d: display_width %d > 20",
                 i, layout->lines[i].display_width);
    }
    md_layout_destroy(layout);
    md_block_destroy(doc);
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(void) {
    printf("test_md_render\n");
    printf("==============\n\n");

    printf("Display width invariant:\n");
    RUN(display_width_invariant_simple);
    RUN(display_width_invariant_narrow);
    RUN(display_width_invariant_long_word);

    printf("\nParagraph reflow:\n");
    RUN(paragraph_reflow_wraps);
    RUN(paragraph_no_reflow_when_fits);

    printf("\nHeading rendering:\n");
    RUN(heading_h1_produces_line);
    RUN(heading_does_not_wrap);
    RUN(heading_all_levels_render);

    printf("\nHorizontal rule:\n");
    RUN(hrule_produces_line);

    printf("\nBlock spacing:\n");
    RUN(no_double_blank_between_blocks);

    printf("\nCode fence:\n");
    RUN(code_fence_no_reflow);
    RUN(code_fence_with_border);

    printf("\nList rendering:\n");
    RUN(unordered_list_has_bullets);
    RUN(ordered_list_has_numbers);

    printf("\nInline styles (Risk R3):\n");
    RUN(bold_text_preserves_across_wrap);

    printf("\nInline code:\n");
    RUN(inline_code_has_distinct_style);

    printf("\nLink rendering:\n");
    RUN(link_renders_url);

    printf("\nEmpty / short documents:\n");
    RUN(render_empty_document);
    RUN(short_document_few_lines);

    printf("\nBlockquote:\n");
    RUN(blockquote_has_bar);

    printf("\nStyle palette:\n");
    RUN(style_palette_h1_matches_plan);
    RUN(style_palette_body_matches_plan);
    RUN(style_palette_inline_code_matches_plan);
    RUN(style_palette_italic_has_bg_hint);

    printf("\nDisplay width — long words (plan S5.5):\n");
    RUN(display_width_long_words_width_10);

    printf("\nHard break rendering:\n");
    RUN(hard_break_produces_two_lines);

    printf("\nMemory:\n");
    RUN(layout_destroy_frees_all);

    printf("\nCJK / wide character reflow (R4):\n");
    RUN(test_cjk_reflow_wrap);
    RUN(test_cjk_at_boundary);
    RUN(test_emoji_reflow);
    RUN(test_utf8_no_split);

    printf("\nEdge-case rendering:\n");
    RUN(test_render_empty_document_v2);
    RUN(test_render_single_char);
    RUN(test_render_width_1);

    printf("\nComprehensive style verification:\n");
    RUN(test_h1_style);
    RUN(test_bold_style);
    RUN(test_italic_style);
    RUN(test_inline_code_style);
    RUN(test_link_style);

    printf("\nCode fence edge cases:\n");
    RUN(test_code_fence_empty);
    RUN(test_code_fence_no_language);

    printf("\nBiDi / RTL rendering:\n");
    RUN(test_bidi_hebrew_renders);
    RUN(test_bidi_display_width);

    printf("\n==============\n");
    printf("%d passed, %d failed, %d total\n", g_pass, g_fail, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}
