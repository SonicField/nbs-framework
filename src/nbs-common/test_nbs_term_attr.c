/*
 * test_nbs_term_attr.c — Test suite for the shared terminal attribute module.
 *
 * These tests define the contract nbs_term_attr must satisfy.
 * The module abstracts xterm 256-colour codes and text attributes
 * so that consumers (nbs-chat-terminal, nbs-chat-edit) never emit
 * raw escape sequences directly.
 *
 * Build:
 *   cc -fsanitize=address,undefined -g -O1 \
 *      -I../nbs-common \
 *      test_nbs_term_attr.c nbs_term_attr.c \
 *      -o test_nbs_term_attr && ./test_nbs_term_attr
 *
 * All tests are self-contained. No external fixtures required.
 */

#include "nbs_term_attr.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Test infrastructure --- */

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    printf(" PASS\n"); \
    g_pass++; \
} while(0)

/* Assert with context — aborts on failure (falsifiable: each can fail) */
#define T_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        printf(" FAIL\n"); \
        fprintf(stderr, "  ASSERTION FAILED %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        g_fail++; \
        return; \
    } \
} while(0)

#define T_STREQ(a, b) T_ASSERT(strcmp((a), (b)) == 0, \
    "expected \"%s\", got \"%s\"", (b), (a))

/* ================================================================
 * 1. STYLE CONSTRUCTION
 * ================================================================ */

TEST(style_default_is_none) {
    nbs_style_t s = NBS_STYLE_INIT;
    T_ASSERT(s.fg == NBS_COLOUR_NONE,
             "default fg should be NBS_COLOUR_NONE, got %d", s.fg);
    T_ASSERT(s.bg == NBS_COLOUR_NONE,
             "default bg should be NBS_COLOUR_NONE, got %d", s.bg);
    T_ASSERT(s.attrs == 0,
             "default attrs should be 0, got %u", s.attrs);
}

TEST(style_set_fg) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.fg = 39;
    T_ASSERT(s.fg == 39, "fg should be 39, got %d", s.fg);
}

TEST(style_set_bg) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.bg = 196;
    T_ASSERT(s.bg == 196, "bg should be 196, got %d", s.bg);
}

TEST(style_set_attrs) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_BOLD | NBS_ATTR_UNDERLINE;
    T_ASSERT(s.attrs == (NBS_ATTR_BOLD | NBS_ATTR_UNDERLINE),
             "attrs mismatch: got %u", s.attrs);
}

/* ================================================================
 * 2. ESCAPE SEQUENCE GENERATION — FOREGROUND COLOURS
 * ================================================================ */

TEST(fg_colour_0) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.fg = 0;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[38;5;0m");
}

TEST(fg_colour_15) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.fg = 15;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[38;5;15m");
}

TEST(fg_colour_255) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.fg = 255;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[38;5;255m");
}

TEST(fg_colour_all_256) {
    /* Verify all 256 foreground colours produce valid escape sequences */
    for (int i = 0; i < 256; i++) {
        nbs_style_t s = NBS_STYLE_INIT;
        s.fg = i;
        char buf[NBS_STYLE_BUFSIZE];
        int n = nbs_style_start(&s, buf, sizeof(buf));
        T_ASSERT(n > 0, "fg=%d: nbs_style_start returned %d", i, n);

        char expected[32];
        snprintf(expected, sizeof(expected), "\033[38;5;%dm", i);
        T_ASSERT(strcmp(buf, expected) == 0,
                 "fg=%d: expected \"%s\", got \"%s\"", i, expected, buf);
    }
}

/* ================================================================
 * 3. ESCAPE SEQUENCE GENERATION — BACKGROUND COLOURS
 * ================================================================ */

TEST(bg_colour_0) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.bg = 0;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[48;5;0m");
}

TEST(bg_colour_255) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.bg = 255;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[48;5;255m");
}

TEST(bg_colour_all_256) {
    for (int i = 0; i < 256; i++) {
        nbs_style_t s = NBS_STYLE_INIT;
        s.bg = i;
        char buf[NBS_STYLE_BUFSIZE];
        int n = nbs_style_start(&s, buf, sizeof(buf));
        T_ASSERT(n > 0, "bg=%d: nbs_style_start returned %d", i, n);

        char expected[32];
        snprintf(expected, sizeof(expected), "\033[48;5;%dm", i);
        T_ASSERT(strcmp(buf, expected) == 0,
                 "bg=%d: expected \"%s\", got \"%s\"", i, expected, buf);
    }
}

/* ================================================================
 * 4. ESCAPE SEQUENCE GENERATION — ATTRIBUTES
 * ================================================================ */

TEST(attr_bold) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_BOLD;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[1m");
}

TEST(attr_dim) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_DIM;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[2m");
}

TEST(attr_italic) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_ITALIC;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[3m");
}

TEST(attr_underline) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_UNDERLINE;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[4m");
}

TEST(attr_blink) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_BLINK;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[5m");
}

TEST(attr_inverse) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_INVERSE;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[7m");
}

TEST(attr_strike) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_STRIKE;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    T_STREQ(buf, "\033[9m");
}

/* ================================================================
 * 5. COMBINED ATTRIBUTES + COLOURS
 * ================================================================ */

TEST(bold_with_fg) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_BOLD;
    s.fg = 39;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    /* Pin exact output: attrs before colours, semicolon-separated */
    T_STREQ(buf, "\033[1;38;5;39m");
}

TEST(bold_underline_with_fg_bg) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_BOLD | NBS_ATTR_UNDERLINE;
    s.fg = 208;
    s.bg = 16;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    /* Pin exact output: attrs (ascending SGR code), then fg, then bg */
    T_STREQ(buf, "\033[1;4;38;5;208;48;5;16m");
}

TEST(all_attrs_combined) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.attrs = NBS_ATTR_BOLD | NBS_ATTR_DIM | NBS_ATTR_ITALIC |
              NBS_ATTR_UNDERLINE | NBS_ATTR_BLINK | NBS_ATTR_INVERSE |
              NBS_ATTR_STRIKE;
    s.fg = 100;
    s.bg = 200;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_start returned %d", n);
    /* Pin exact output: all 7 attrs in SGR order, then fg, then bg */
    T_STREQ(buf, "\033[1;2;3;4;5;7;9;38;5;100;48;5;200m");
    /* Buffer fits */
    T_ASSERT(n < NBS_STYLE_BUFSIZE,
             "output length %d exceeds NBS_STYLE_BUFSIZE %d",
             n, NBS_STYLE_BUFSIZE);
}

/* ================================================================
 * 6. RESET SEQUENCE
 * ================================================================ */

TEST(reset_sequence) {
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_reset(buf, sizeof(buf));
    T_ASSERT(n > 0, "nbs_style_reset returned %d", n);
    T_STREQ(buf, "\033[0m");
}

/* ================================================================
 * 7. NO-OP STYLE (no attrs, no colours)
 * ================================================================ */

TEST(empty_style_produces_nothing) {
    nbs_style_t s = NBS_STYLE_INIT;
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&s, buf, sizeof(buf));
    /* With no attrs and no colours, should produce empty string or 0 bytes */
    T_ASSERT(n == 0, "empty style should produce 0 bytes, got %d", n);
    T_ASSERT(buf[0] == '\0', "buf should be empty string");
}

/* ================================================================
 * 8. BUFFER SIZE BOUNDARY CONDITIONS
 * ================================================================ */

TEST(buffer_too_small_returns_error) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.fg = 255;
    s.bg = 255;
    s.attrs = NBS_ATTR_BOLD | NBS_ATTR_UNDERLINE;
    char buf[4]; /* Far too small */
    int n = nbs_style_start(&s, buf, sizeof(buf));
    T_ASSERT(n < 0, "should return error for tiny buffer, got %d", n);
}

TEST(buffer_exact_fit) {
    /* Reset is the shortest possible output: \033[0m = 4 chars + NUL = 5 */
    char buf[5];
    int n = nbs_style_reset(buf, sizeof(buf));
    T_ASSERT(n == 4, "reset should be 4 bytes, got %d", n);
    T_STREQ(buf, "\033[0m");
}

TEST(buffer_one_short_for_reset) {
    char buf[4]; /* Need 5 for \033[0m\0 */
    int n = nbs_style_reset(buf, sizeof(buf));
    T_ASSERT(n < 0, "should return error when buffer is 1 byte short, got %d", n);
}

/* ================================================================
 * 9. HANDLE-TO-COLOUR MAPPING
 * ================================================================ */

TEST(handle_colour_init_resets) {
    nbs_handle_colours_init();
    /* After init, first handle should get the first palette colour */
    const nbs_style_t *s1 = nbs_handle_colour("alice");
    T_ASSERT(s1 != NULL, "nbs_handle_colour returned NULL");
    nbs_handle_colours_init();
    const nbs_style_t *s2 = nbs_handle_colour("alice");
    T_ASSERT(s2 != NULL, "nbs_handle_colour returned NULL after re-init");
    /* After re-init, alice should get the same first-palette colour */
    T_ASSERT(s1->fg == s2->fg,
             "after re-init alice should get same colour: %d vs %d",
             s1->fg, s2->fg);
}

TEST(handle_colour_stability) {
    nbs_handle_colours_init();
    const nbs_style_t *s1 = nbs_handle_colour("bob");
    const nbs_style_t *s2 = nbs_handle_colour("bob");
    T_ASSERT(s1 == s2, "same handle should return same pointer");
}

TEST(handle_colour_different_handles_get_different_colours) {
    nbs_handle_colours_init();
    const nbs_style_t *a = nbs_handle_colour("alice");
    const nbs_style_t *b = nbs_handle_colour("bob");
    T_ASSERT(a->fg != b->fg,
             "alice and bob should get different fg colours: %d vs %d",
             a->fg, b->fg);
}

TEST(handle_colour_wraps_around) {
    nbs_handle_colours_init();
    /* Get styles for more handles than palette entries.
     * The palette should wrap around without crashing. */
    const nbs_style_t *first = nbs_handle_colour("handle_0");
    int first_fg = first->fg;
    /* Insert enough handles to wrap the palette */
    for (int i = 1; i < 20; i++) {
        char h[32];
        snprintf(h, sizeof(h), "handle_%d", i);
        const nbs_style_t *s = nbs_handle_colour(h);
        T_ASSERT(s != NULL, "handle_%d returned NULL", i);
        T_ASSERT(s->fg >= 0 && s->fg <= 255,
                 "handle_%d: fg %d out of [0,255]", i, s->fg);
    }
    /* After wrapping, some handle should share a colour with handle_0 */
    /* (This verifies wrap-around actually happened) */
    int found_wrap = 0;
    for (int i = 1; i < 20; i++) {
        char h[32];
        snprintf(h, sizeof(h), "handle_%d", i);
        const nbs_style_t *s = nbs_handle_colour(h);
        if (s->fg == first_fg) { found_wrap = 1; break; }
    }
    T_ASSERT(found_wrap, "palette should wrap — no handle shares fg with handle_0");
}

TEST(handle_colour_max_participants) {
    nbs_handle_colours_init();
    /* Fill all participant slots */
    for (int i = 0; i < NBS_MAX_HANDLE_COLOURS; i++) {
        char h[32];
        snprintf(h, sizeof(h), "user_%04d", i);
        const nbs_style_t *s = nbs_handle_colour(h);
        T_ASSERT(s != NULL, "user_%04d returned NULL", i);
    }
    /* One more should still return a valid style (fallback) */
    const nbs_style_t *overflow = nbs_handle_colour("overflow_user");
    T_ASSERT(overflow != NULL, "overflow handle returned NULL");
    T_ASSERT(overflow->fg >= 0 && overflow->fg <= 255,
             "overflow fg %d out of range", overflow->fg);
}

TEST(handle_colour_empty_handle) {
    nbs_handle_colours_init();
    /* Empty string is a valid (if unusual) handle */
    const nbs_style_t *s = nbs_handle_colour("");
    T_ASSERT(s != NULL, "empty handle returned NULL");
}

/* ================================================================
 * 10. CONVENIENCE: FILE OUTPUT
 * ================================================================ */

TEST(fstart_writes_to_file) {
    nbs_style_t s = NBS_STYLE_INIT;
    s.fg = 41;
    s.attrs = NBS_ATTR_BOLD;

    char membuf[256];
    FILE *f = fmemopen(membuf, sizeof(membuf), "w");
    T_ASSERT(f != NULL, "fmemopen failed");
    nbs_style_fstart(&s, f);
    fflush(f);
    fclose(f);

    /* Pin exact output */
    T_STREQ(membuf, "\033[1;38;5;41m");
}

TEST(freset_writes_to_file) {
    char membuf[64];
    FILE *f = fmemopen(membuf, sizeof(membuf), "w");
    T_ASSERT(f != NULL, "fmemopen failed");
    nbs_style_freset(f);
    fflush(f);
    fclose(f);
    T_STREQ(membuf, "\033[0m");
}

/* ================================================================
 * 11. PREDEFINED STYLE CONSTANTS
 *
 * The module should provide named constants for common styles
 * matching what editor.c and render.c currently hardcode.
 * ================================================================ */

TEST(predefined_bold) {
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&NBS_STYLE_BOLD, buf, sizeof(buf));
    T_ASSERT(n > 0, "NBS_STYLE_BOLD start returned %d", n);
    T_STREQ(buf, "\033[1m");
}

TEST(predefined_dim) {
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&NBS_STYLE_DIM, buf, sizeof(buf));
    T_ASSERT(n > 0, "NBS_STYLE_DIM start returned %d", n);
    T_STREQ(buf, "\033[2m");
}

TEST(predefined_reverse) {
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&NBS_STYLE_REVERSE, buf, sizeof(buf));
    T_ASSERT(n > 0, "NBS_STYLE_REVERSE start returned %d", n);
    T_STREQ(buf, "\033[7m");
}

TEST(predefined_strike) {
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&NBS_STYLE_STRIKE, buf, sizeof(buf));
    T_ASSERT(n > 0, "NBS_STYLE_STRIKE start returned %d", n);
    T_STREQ(buf, "\033[9m");
}

/* ================================================================
 * 12. SEMANTIC UI STYLE CONSTANTS
 *
 * These replace the 16-colour C_RED/C_GREEN/etc macros in editor.c
 * with 256-colour styles from the handle palette.
 * ================================================================ */

TEST(semantic_error_is_red) {
    T_ASSERT(NBS_STYLE_ERROR.fg == 196,
             "ERROR fg should be 196 (red), got %d", NBS_STYLE_ERROR.fg);
    T_ASSERT(NBS_STYLE_ERROR.bg == NBS_COLOUR_NONE,
             "ERROR bg should be NONE, got %d", NBS_STYLE_ERROR.bg);
    T_ASSERT(NBS_STYLE_ERROR.attrs == 0,
             "ERROR attrs should be 0, got %u", NBS_STYLE_ERROR.attrs);
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&NBS_STYLE_ERROR, buf, sizeof(buf));
    T_ASSERT(n > 0, "start returned %d", n);
    T_STREQ(buf, "\033[38;5;196m");
}

TEST(semantic_warning_is_yellow) {
    T_ASSERT(NBS_STYLE_WARNING.fg == 226,
             "WARNING fg should be 226 (yellow), got %d", NBS_STYLE_WARNING.fg);
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&NBS_STYLE_WARNING, buf, sizeof(buf));
    T_ASSERT(n > 0, "start returned %d", n);
    T_STREQ(buf, "\033[38;5;226m");
}

TEST(semantic_info_is_cyan) {
    T_ASSERT(NBS_STYLE_INFO.fg == 87,
             "INFO fg should be 87 (cyan), got %d", NBS_STYLE_INFO.fg);
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&NBS_STYLE_INFO, buf, sizeof(buf));
    T_ASSERT(n > 0, "start returned %d", n);
    T_STREQ(buf, "\033[38;5;87m");
}

TEST(semantic_success_is_green) {
    T_ASSERT(NBS_STYLE_SUCCESS.fg == 41,
             "SUCCESS fg should be 41 (green), got %d", NBS_STYLE_SUCCESS.fg);
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(&NBS_STYLE_SUCCESS, buf, sizeof(buf));
    T_ASSERT(n > 0, "start returned %d", n);
    T_STREQ(buf, "\033[38;5;41m");
}

/* ================================================================
 * 13. PALETTE QUERY
 *
 * The module should expose its handle colour palette for tests
 * and for consumers that need to know the available colours.
 * ================================================================ */

TEST(palette_size_nonzero) {
    int size = nbs_handle_palette_size();
    T_ASSERT(size > 0, "palette size should be > 0, got %d", size);
}

TEST(palette_colours_valid) {
    int size = nbs_handle_palette_size();
    for (int i = 0; i < size; i++) {
        const nbs_style_t *s = nbs_handle_palette_entry(i);
        T_ASSERT(s != NULL, "palette entry %d is NULL", i);
        T_ASSERT(s->fg >= 0 && s->fg <= 255,
                 "palette[%d].fg = %d, out of [0,255]", i, s->fg);
    }
}

/* ================================================================
 * RUNNER
 * ================================================================ */

int main(void) {
    printf("test_nbs_term_attr\n");
    printf("==================\n\n");

    printf("Style construction:\n");
    RUN(style_default_is_none);
    RUN(style_set_fg);
    RUN(style_set_bg);
    RUN(style_set_attrs);

    printf("\nForeground colours:\n");
    RUN(fg_colour_0);
    RUN(fg_colour_15);
    RUN(fg_colour_255);
    RUN(fg_colour_all_256);

    printf("\nBackground colours:\n");
    RUN(bg_colour_0);
    RUN(bg_colour_255);
    RUN(bg_colour_all_256);

    printf("\nAttributes:\n");
    RUN(attr_bold);
    RUN(attr_dim);
    RUN(attr_italic);
    RUN(attr_underline);
    RUN(attr_blink);
    RUN(attr_inverse);
    RUN(attr_strike);

    printf("\nCombined attributes + colours:\n");
    RUN(bold_with_fg);
    RUN(bold_underline_with_fg_bg);
    RUN(all_attrs_combined);

    printf("\nReset:\n");
    RUN(reset_sequence);

    printf("\nEmpty / no-op style:\n");
    RUN(empty_style_produces_nothing);

    printf("\nBuffer boundaries:\n");
    RUN(buffer_too_small_returns_error);
    RUN(buffer_exact_fit);
    RUN(buffer_one_short_for_reset);

    printf("\nHandle-to-colour mapping:\n");
    RUN(handle_colour_init_resets);
    RUN(handle_colour_stability);
    RUN(handle_colour_different_handles_get_different_colours);
    RUN(handle_colour_wraps_around);
    RUN(handle_colour_max_participants);
    RUN(handle_colour_empty_handle);

    printf("\nFile output:\n");
    RUN(fstart_writes_to_file);
    RUN(freset_writes_to_file);

    printf("\nPredefined styles:\n");
    RUN(predefined_bold);
    RUN(predefined_dim);
    RUN(predefined_reverse);
    RUN(predefined_strike);

    printf("\nSemantic UI styles:\n");
    RUN(semantic_error_is_red);
    RUN(semantic_warning_is_yellow);
    RUN(semantic_info_is_cyan);
    RUN(semantic_success_is_green);

    printf("\nPalette query:\n");
    RUN(palette_size_nonzero);
    RUN(palette_colours_valid);

    printf("\n==================\n");
    printf("%d passed, %d failed, %d total\n", g_pass, g_fail, g_pass + g_fail);

    return g_fail > 0 ? 1 : 0;
}
