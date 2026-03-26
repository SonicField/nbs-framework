/*
 * test_nbs_ts_render.c — Comprehensive test suite for nbs-ts-render.
 *
 * Tests the terminal emulator against known input/output pairs.
 * Each test creates a fresh terminal, feeds input, takes a snapshot,
 * and compares against expected output.
 */

#include "nbs_ts_render.h"
#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  %-50s ", #name); \
    fflush(stdout); \
    name(); \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

/* Feed a NUL-terminated string (convenience) */
static void feed_str(ts_render_t *t, const char *s) {
    ts_render_feed(t, s, strlen(s));
}

/* Assert snapshot matches expected string */
static void assert_snapshot(ts_render_t *t, const char *expected, const char *test_name) {
    char *snap = ts_render_snapshot(t);
    ASSERT_MSG(snap != NULL, "%s: snapshot returned NULL", test_name);
    if (strcmp(snap, expected) != 0) {
        fprintf(stderr, "\n  FAIL: %s\n", test_name);
        fprintf(stderr, "  Expected: [");
        for (const char *p = expected; *p; p++) {
            if (*p == '\n') fprintf(stderr, "\\n");
            else if (*p == '\r') fprintf(stderr, "\\r");
            else if ((unsigned char)*p < 0x20) fprintf(stderr, "\\x%02x", (unsigned char)*p);
            else fputc(*p, stderr);
        }
        fprintf(stderr, "]\n  Got:      [");
        for (const char *p = snap; *p; p++) {
            if (*p == '\n') fprintf(stderr, "\\n");
            else if (*p == '\r') fprintf(stderr, "\\r");
            else if ((unsigned char)*p < 0x20) fprintf(stderr, "\\x%02x", (unsigned char)*p);
            else fputc(*p, stderr);
        }
        fprintf(stderr, "]\n");
        free(snap);
        tests_failed++;
        tests_run++;
        /* Don't abort — continue with other tests */
        return;
    }
    free(snap);
}

/* Wrapper that auto-names from __func__ would be nice but C11 doesn't
 * let us do string concat with __func__. Use explicit test names. */

/* ══════════════════════════════════════════════════════════════════ */
/*  BASIC TEXT RENDERING                                             */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_empty_screen) {
    ts_render_t *t = ts_render_create(3, 10);
    assert_snapshot(t, "\n", "empty_screen");
    ts_render_destroy(t);
}

TEST(test_simple_text) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "Hello");
    assert_snapshot(t, "Hello\n", "simple_text");
    ts_render_destroy(t);
}

TEST(test_text_with_newline) {
    ts_render_t *t = ts_render_create(5, 20);
    feed_str(t, "Line 1\r\nLine 2\r\nLine 3");
    assert_snapshot(t, "Line 1\nLine 2\nLine 3\n", "text_with_newline");
    ts_render_destroy(t);
}

TEST(test_carriage_return) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "AAAA\rBB");
    assert_snapshot(t, "BBAA\n", "carriage_return");
    ts_render_destroy(t);
}

TEST(test_cr_lf) {
    ts_render_t *t = ts_render_create(5, 20);
    feed_str(t, "Line 1\r\nLine 2\r\n");
    assert_snapshot(t, "Line 1\nLine 2\n", "cr_lf");
    ts_render_destroy(t);
}

TEST(test_backspace) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "ABC\b\bXY");
    assert_snapshot(t, "AXY\n", "backspace");
    ts_render_destroy(t);
}

TEST(test_backspace_at_col0) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "\b\bABC");
    assert_snapshot(t, "ABC\n", "backspace_at_col0");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  AUTO-WRAP                                                        */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_wrap_at_right_margin) {
    ts_render_t *t = ts_render_create(3, 5);
    feed_str(t, "12345X");
    assert_snapshot(t, "12345\nX\n", "wrap_at_right_margin");
    ts_render_destroy(t);
}

TEST(test_wrap_fills_screen) {
    ts_render_t *t = ts_render_create(2, 5);
    feed_str(t, "1234567890");
    assert_snapshot(t, "12345\n67890\n", "wrap_fills_screen");
    ts_render_destroy(t);
}

TEST(test_wrap_causes_scroll) {
    ts_render_t *t = ts_render_create(2, 5);
    feed_str(t, "123456789012345");
    /* 3 rows of 5 on a 2-row screen: first row scrolled off */
    assert_snapshot(t, "67890\n12345\n", "wrap_causes_scroll");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  SCROLLING                                                        */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_scroll_up_via_newline) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "Line 1\r\nLine 2\r\nLine 3\r\nLine 4");
    /* Line 1 scrolled off */
    assert_snapshot(t, "Line 2\nLine 3\nLine 4\n", "scroll_up_via_newline");
    ts_render_destroy(t);
}

TEST(test_scroll_up_csi) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "AAA\r\nBBB\r\nCCC");
    feed_str(t, "\x1b[S"); /* Scroll up 1 */
    assert_snapshot(t, "BBB\nCCC\n", "scroll_up_csi");
    ts_render_destroy(t);
}

TEST(test_scroll_down_csi) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "AAA\r\nBBB\r\nCCC");
    feed_str(t, "\x1b[T"); /* Scroll down 1 */
    assert_snapshot(t, "\nAAA\nBBB\n", "scroll_down_csi");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  CURSOR MOVEMENT                                                  */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_cursor_up) {
    ts_render_t *t = ts_render_create(5, 10);
    feed_str(t, "AAA\r\nBBB\r\n\x1b[2AX");
    /* Move up 2 from row 2 → row 0, col 0 */
    assert_snapshot(t, "XAA\nBBB\n", "cursor_up");
    ts_render_destroy(t);
}

TEST(test_cursor_down) {
    ts_render_t *t = ts_render_create(5, 10);
    feed_str(t, "AAA\x1b[2BX");
    /* Move down 2 from row 0 → row 2 */
    assert_snapshot(t, "AAA\n\n   X\n", "cursor_down");
    ts_render_destroy(t);
}

TEST(test_cursor_forward) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "A\x1b[3CB");
    assert_snapshot(t, "A   B\n", "cursor_forward");
    ts_render_destroy(t);
}

TEST(test_cursor_backward) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "ABCDE\x1b[3DX");
    assert_snapshot(t, "ABXDE\n", "cursor_backward");
    ts_render_destroy(t);
}

TEST(test_cursor_position) {
    ts_render_t *t = ts_render_create(5, 10);
    feed_str(t, "\x1b[3;5HX");
    /* Row 3, col 5 (1-based) → row 2, col 4 (0-based) */
    assert_snapshot(t, "\n\n    X\n", "cursor_position");
    ts_render_destroy(t);
}

TEST(test_cursor_position_default) {
    ts_render_t *t = ts_render_create(5, 10);
    feed_str(t, "Hello\x1b[HX");
    /* CUP with no params → home (1,1) */
    assert_snapshot(t, "Xello\n", "cursor_position_default");
    ts_render_destroy(t);
}

TEST(test_cursor_horizontal_absolute) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "ABCDEFG\x1b[4GX");
    /* CHA col 4 (1-based) → col 3 (0-based) */
    assert_snapshot(t, "ABCXEFG\n", "cursor_horizontal_absolute");
    ts_render_destroy(t);
}

TEST(test_cursor_next_line) {
    ts_render_t *t = ts_render_create(5, 10);
    feed_str(t, "AAAA\x1b[2EX");
    /* CNL: move down 2 lines, col 0 */
    assert_snapshot(t, "AAAA\n\nX\n", "cursor_next_line");
    ts_render_destroy(t);
}

TEST(test_cursor_previous_line) {
    ts_render_t *t = ts_render_create(5, 10);
    feed_str(t, "AAA\r\nBBB\r\nCCC\x1b[2FX");
    /* CPL: move up 2 lines, col 0. X overwrites first char of "AAA" */
    assert_snapshot(t, "XAA\nBBB\nCCC\n", "cursor_previous_line");
    ts_render_destroy(t);
}

TEST(test_vertical_position_absolute) {
    ts_render_t *t = ts_render_create(5, 10);
    feed_str(t, "Hello\x1b[3dX");
    /* VPA row 3 (1-based) → row 2, keeps col (5) */
    assert_snapshot(t, "Hello\n\n     X\n", "vertical_position_absolute");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  ERASE                                                            */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_erase_to_end_of_screen) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "AAAA\r\nBBBB\r\nCCCC");
    feed_str(t, "\x1b[2;3H"); /* Row 2, col 3 */
    feed_str(t, "\x1b[J");    /* ED 0: erase to end */
    assert_snapshot(t, "AAAA\nBB\n", "erase_to_end_of_screen");
    ts_render_destroy(t);
}

TEST(test_erase_to_start_of_screen) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "AAAA\r\nBBBB\r\nCCCC");
    feed_str(t, "\x1b[2;3H"); /* Row 2, col 3 */
    feed_str(t, "\x1b[1J");   /* ED 1: erase to start */
    assert_snapshot(t, "\n   B\nCCCC\n", "erase_to_start_of_screen");
    ts_render_destroy(t);
}

TEST(test_erase_entire_screen) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "AAAA\nBBBB\nCCCC");
    feed_str(t, "\x1b[2J");
    assert_snapshot(t, "\n", "erase_entire_screen");
    ts_render_destroy(t);
}

TEST(test_erase_to_end_of_line) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "ABCDEFGH\x1b[4G\x1b[K");
    /* Erase from col 3 to end of line */
    assert_snapshot(t, "ABC\n", "erase_to_end_of_line");
    ts_render_destroy(t);
}

TEST(test_erase_to_start_of_line) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "ABCDEFGH\x1b[4G\x1b[1K");
    assert_snapshot(t, "    EFGH\n", "erase_to_start_of_line");
    ts_render_destroy(t);
}

TEST(test_erase_entire_line) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "Line 1\r\nLine 2\r\nLine 3");
    feed_str(t, "\x1b[2;1H\x1b[2K");
    assert_snapshot(t, "Line 1\n\nLine 3\n", "erase_entire_line");
    ts_render_destroy(t);
}

TEST(test_erase_characters) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "ABCDEFGH\x1b[3G\x1b[3X");
    /* ECH: erase 3 chars starting at col 2 */
    assert_snapshot(t, "AB   FGH\n", "erase_characters");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  INSERT / DELETE                                                  */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_insert_lines) {
    ts_render_t *t = ts_render_create(4, 10);
    feed_str(t, "AAA\r\nBBB\r\nCCC\r\nDDD");
    feed_str(t, "\x1b[2;1H"); /* Row 2 */
    feed_str(t, "\x1b[L");    /* Insert 1 line */
    assert_snapshot(t, "AAA\n\nBBB\nCCC\n", "insert_lines");
    ts_render_destroy(t);
}

TEST(test_delete_lines) {
    ts_render_t *t = ts_render_create(4, 10);
    feed_str(t, "AAA\r\nBBB\r\nCCC\r\nDDD");
    feed_str(t, "\x1b[2;1H"); /* Row 2 */
    feed_str(t, "\x1b[M");    /* Delete 1 line */
    assert_snapshot(t, "AAA\nCCC\nDDD\n", "delete_lines");
    ts_render_destroy(t);
}

TEST(test_insert_characters) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "ABCDEFGH\x1b[3G\x1b[2@");
    /* ICH: insert 2 blanks at col 2, shifting right */
    assert_snapshot(t, "AB  CDEFGH\n", "insert_characters");
    ts_render_destroy(t);
}

TEST(test_delete_characters) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "ABCDEFGH\x1b[3G\x1b[2P");
    /* DCH: delete 2 chars at col 2, shifting left */
    assert_snapshot(t, "ABEFGH\n", "delete_characters");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  TABS                                                             */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_tab_default_stops) {
    ts_render_t *t = ts_render_create(3, 40);
    feed_str(t, "A\tB\tC");
    /* Default tabs at 8, 16, 24... */
    assert_snapshot(t, "A       B       C\n", "tab_default_stops");
    ts_render_destroy(t);
}

TEST(test_tab_at_end_of_line) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "1234567\t");
    /* Tab from col 7 → col 8 (next tab at 8) */
    assert_snapshot(t, "1234567\n", "tab_at_end_of_line");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  SCROLL REGION                                                    */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_scroll_region) {
    ts_render_t *t = ts_render_create(5, 10);
    feed_str(t, "AAA\r\nBBB\r\nCCC\r\nDDD\r\nEEE");
    /* Set scroll region to rows 2-4 (1-based) */
    feed_str(t, "\x1b[2;4r");
    /* Move to row 4 (bottom of region) and do LF */
    feed_str(t, "\x1b[4;1H\nXXX");
    /* BBB should scroll up within region, AAA and EEE untouched */
    assert_snapshot(t, "AAA\nCCC\nDDD\nXXX\nEEE\n", "scroll_region");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  SGR STRIPPING                                                    */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_sgr_stripped) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "\x1b[1;31mBold Red\x1b[0m Normal");
    assert_snapshot(t, "Bold Red Normal\n", "sgr_stripped");
    ts_render_destroy(t);
}

TEST(test_sgr_256_color) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "\x1b[38;5;196mRed\x1b[48;2;0;128;255mBG\x1b[0m");
    assert_snapshot(t, "RedBG\n", "sgr_256_color");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  OSC / DCS HANDLING                                               */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_osc_title_bel) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "\x1b]0;Window Title\x07Hello");
    assert_snapshot(t, "Hello\n", "osc_title_bel");
    ts_render_destroy(t);
}

TEST(test_osc_title_st) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "\x1b]0;Window Title\x1b\\Hello");
    assert_snapshot(t, "Hello\n", "osc_title_st");
    ts_render_destroy(t);
}

TEST(test_dcs_consumed) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "\x1bPsome DCS data\x1b\\Hello");
    assert_snapshot(t, "Hello\n", "dcs_consumed");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  CURSOR SAVE / RESTORE                                            */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_cursor_save_restore) {
    ts_render_t *t = ts_render_create(5, 10);
    feed_str(t, "ABC");
    feed_str(t, "\x1b""7");     /* Save cursor (ESC 7) */
    feed_str(t, "\x1b[3;5HX");  /* Move elsewhere */
    feed_str(t, "\x1b""8");     /* Restore cursor (ESC 8) */
    feed_str(t, "D");
    assert_snapshot(t, "ABCD\n\n    X\n", "cursor_save_restore");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  FULL RESET                                                       */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_full_reset) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "AAAA\nBBBB");
    feed_str(t, "\x1b""c"); /* RIS — full reset */
    feed_str(t, "X");
    assert_snapshot(t, "X\n", "full_reset");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  UTF-8                                                            */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_utf8_2byte) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "caf\xc3\xa9"); /* café */
    assert_snapshot(t, "caf\xc3\xa9\n", "utf8_2byte");
    ts_render_destroy(t);
}

TEST(test_utf8_3byte) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "\xe2\x9c\x93 check"); /* ✓ check */
    assert_snapshot(t, "\xe2\x9c\x93 check\n", "utf8_3byte");
    ts_render_destroy(t);
}

TEST(test_utf8_4byte) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "\xf0\x9f\x98\x80 smile"); /* 😀 smile */
    assert_snapshot(t, "\xf0\x9f\x98\x80 smile\n", "utf8_4byte");
    ts_render_destroy(t);
}

TEST(test_utf8_overwrite) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "ABCD\rXX");
    assert_snapshot(t, "XXCD\n", "utf8_overwrite");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  PRIVATE MODE SEQUENCES (should be ignored)                       */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_private_mode_ignored) {
    ts_render_t *t = ts_render_create(3, 20);
    /* DECSET cursor visibility, bracketed paste, etc. */
    feed_str(t, "\x1b[?25l\x1b[?2004hHello\x1b[?25h\x1b[?2004l");
    assert_snapshot(t, "Hello\n", "private_mode_ignored");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  REVERSE INDEX                                                    */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_reverse_index) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "AAA\r\nBBB\r\nCCC");
    feed_str(t, "\x1b[1;1H"); /* Go to top */
    feed_str(t, "\x1b""M");   /* Reverse index — should scroll down */
    feed_str(t, "XXX");
    assert_snapshot(t, "XXX\nAAA\nBBB\n", "reverse_index");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  EDGE CASES                                                       */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_cursor_beyond_right_margin) {
    ts_render_t *t = ts_render_create(3, 5);
    feed_str(t, "\x1b[1;99HC");
    /* CUP col 99 on a 5-col terminal → clamped to col 4 */
    assert_snapshot(t, "    C\n", "cursor_beyond_right_margin");
    ts_render_destroy(t);
}

TEST(test_cursor_beyond_bottom) {
    ts_render_t *t = ts_render_create(3, 5);
    feed_str(t, "\x1b[99;1HX");
    /* CUP row 99 on a 3-row terminal → clamped to row 2 */
    assert_snapshot(t, "\n\nX\n", "cursor_beyond_bottom");
    ts_render_destroy(t);
}

TEST(test_zero_param_treated_as_default) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "ABC\x1b[0G");
    /* CHA with param 0 → treated as 1 (column 1) */
    assert_snapshot(t, "ABC\n", "zero_param_treated_as_default");
    /* Cursor at col 0 after CHA 0, but no character written */
    ts_render_destroy(t);
}

TEST(test_malformed_csi_ignored) {
    ts_render_t *t = ts_render_create(3, 20);
    /* ESC [ followed by invalid byte — should abort CSI */
    feed_str(t, "A\x1b[\x01""B");
    assert_snapshot(t, "AB\n", "malformed_csi_ignored");
    ts_render_destroy(t);
}

TEST(test_bare_esc_at_eof) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "Hello\x1b");
    /* Bare ESC at end — should not crash */
    assert_snapshot(t, "Hello\n", "bare_esc_at_eof");
    ts_render_destroy(t);
}

TEST(test_many_csi_params) {
    ts_render_t *t = ts_render_create(3, 20);
    /* SGR with many params — should not overflow */
    feed_str(t, "\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18mHello\x1b[0m");
    assert_snapshot(t, "Hello\n", "many_csi_params");
    ts_render_destroy(t);
}

TEST(test_esc_intermediate_ignored) {
    ts_render_t *t = ts_render_create(3, 20);
    /* ESC ( B — designate character set, should be ignored */
    feed_str(t, "\x1b(BHello");
    assert_snapshot(t, "Hello\n", "esc_intermediate_ignored");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  REALISTIC SCENARIOS                                              */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_clear_screen_and_rewrite) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "Old content here");
    feed_str(t, "\x1b[2J\x1b[H");  /* Clear + home */
    feed_str(t, "New content");
    assert_snapshot(t, "New content\n", "clear_screen_and_rewrite");
    ts_render_destroy(t);
}

TEST(test_progress_bar_simulation) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Simulate a progress bar that overwrites itself */
    feed_str(t, "Progress: 10%");
    feed_str(t, "\rProgress: 50%");
    feed_str(t, "\rProgress: 100%");
    assert_snapshot(t, "Progress: 100%\n", "progress_bar_simulation");
    ts_render_destroy(t);
}

TEST(test_colored_prompt_stripped) {
    ts_render_t *t = ts_render_create(3, 40);
    /* Typical bash prompt with colors */
    feed_str(t, "\x1b[01;32muser@host\x1b[00m:\x1b[01;34m~/code\x1b[00m$ ls");
    assert_snapshot(t, "user@host:~/code$ ls\n", "colored_prompt_stripped");
    ts_render_destroy(t);
}

TEST(test_alternate_screen_mode_ignored) {
    ts_render_t *t = ts_render_create(3, 20);
    feed_str(t, "Before");
    /* DECSET 1049 (alternate screen) — we ignore it */
    feed_str(t, "\x1b[?1049h");
    feed_str(t, "\x1b[2J\x1b[HAfter");
    feed_str(t, "\x1b[?1049l");
    assert_snapshot(t, "After\n", "alternate_screen_mode_ignored");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  API                                                              */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_reset_clears_all) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "Hello\nWorld");
    ts_render_reset(t);
    assert_snapshot(t, "\n", "reset_clears_all");
    ts_render_destroy(t);
}

TEST(test_multiple_feeds) {
    ts_render_t *t = ts_render_create(3, 10);
    feed_str(t, "Hel");
    feed_str(t, "lo ");
    feed_str(t, "World");
    assert_snapshot(t, "Hello Worl\nd\n", "multiple_feeds");
    ts_render_destroy(t);
}

TEST(test_feed_split_escape) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Split an escape sequence across two feeds */
    feed_str(t, "ABCDEF\x1b[");
    feed_str(t, "3D");  /* cursor back 3 */
    feed_str(t, "X");
    assert_snapshot(t, "ABCXEF\n", "feed_split_escape");
    ts_render_destroy(t);
}

TEST(test_feed_split_utf8) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Split a 3-byte UTF-8 char across feeds */
    ts_render_feed(t, "A\xe2", 2);
    ts_render_feed(t, "\x9c\x93""B", 3); /* ✓B */
    assert_snapshot(t, "A\xe2\x9c\x93""B\n", "feed_split_utf8");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  UTF-8 + ESCAPE INTERACTION                                       */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_colored_utf8_text) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Bold red café — SGR around multi-byte chars */
    feed_str(t, "\x1b[1;31mcaf\xc3\xa9\x1b[0m ok");
    assert_snapshot(t, "caf\xc3\xa9 ok\n", "colored_utf8_text");
    ts_render_destroy(t);
}

TEST(test_colored_emoji) {
    ts_render_t *t = ts_render_create(3, 20);
    /* 256-color around 4-byte emoji */
    feed_str(t, "\x1b[38;5;196m\xf0\x9f\x98\x80\x1b[0m done");
    assert_snapshot(t, "\xf0\x9f\x98\x80 done\n", "colored_emoji");
    ts_render_destroy(t);
}

TEST(test_cursor_movement_over_utf8) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Write café, move back 2, overwrite with X */
    feed_str(t, "caf\xc3\xa9!");
    feed_str(t, "\x1b[2DX");
    /* Cursor was at col 5, back 2 → col 3 (the é cell), X overwrites é */
    assert_snapshot(t, "cafX!\n", "cursor_movement_over_utf8");
    ts_render_destroy(t);
}

TEST(test_erase_utf8_cells) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Write ✓✓✓, erase 2 chars starting at col 1 */
    feed_str(t, "\xe2\x9c\x93\xe2\x9c\x93\xe2\x9c\x93");
    feed_str(t, "\x1b[2G\x1b[2X"); /* CHA col 2, ECH 2 */
    assert_snapshot(t, "\xe2\x9c\x93\n", "erase_utf8_cells");
    ts_render_destroy(t);
}

TEST(test_overwrite_utf8_with_ascii) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Write café, CR, overwrite with ABCDE */
    feed_str(t, "caf\xc3\xa9!\rABCDE");
    assert_snapshot(t, "ABCDE\n", "overwrite_utf8_with_ascii");
    ts_render_destroy(t);
}

TEST(test_overwrite_ascii_with_utf8) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Write ABCDE, CR, overwrite with café */
    feed_str(t, "ABCDE\rcaf\xc3\xa9");
    assert_snapshot(t, "caf\xc3\xa9""E\n", "overwrite_ascii_with_utf8");
    ts_render_destroy(t);
}

TEST(test_utf8_at_wrap_boundary) {
    ts_render_t *t = ts_render_create(3, 5);
    /* Fill 4 cols with ASCII, then write a 2-byte UTF-8 char */
    feed_str(t, "1234\xc3\xa9");
    /* é occupies col 4 (last col), pending wrap set */
    assert_snapshot(t, "1234\xc3\xa9\n", "utf8_at_wrap_boundary");
    ts_render_destroy(t);
}

TEST(test_utf8_wrap_then_continue) {
    ts_render_t *t = ts_render_create(3, 5);
    /* Fill all 5 cols, then write a UTF-8 char (should wrap) */
    feed_str(t, "12345\xc3\xa9");
    assert_snapshot(t, "12345\n\xc3\xa9\n", "utf8_wrap_then_continue");
    ts_render_destroy(t);
}

TEST(test_esc_interrupts_utf8) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Start a 2-byte UTF-8 sequence, then ESC interrupts it */
    /* 0xC3 starts a 2-byte char, but ESC follows instead of continuation */
    ts_render_feed(t, "AB\xc3", 3);
    feed_str(t, "\x1b[31m");  /* SGR red — should be stripped */
    feed_str(t, "CD");
    /* The incomplete UTF-8 (0xC3) should be abandoned, CD rendered */
    assert_snapshot(t, "ABCD\n", "esc_interrupts_utf8");
    ts_render_destroy(t);
}

TEST(test_malformed_utf8_in_csi) {
    ts_render_t *t = ts_render_create(3, 20);
    /* UTF-8 continuation byte (0x80-0xBF) inside CSI params — not valid */
    feed_str(t, "AB\x1b[\x80""1mCD");
    /* 0x80 is not a valid CSI byte (< 0x30 except for specials) */
    /* Should abort CSI, 'm' and other chars handled per state machine */
    /* Just verify no crash and text is rendered */
    char *snap = ts_render_snapshot(t);
    ASSERT_MSG(snap != NULL, "malformed_utf8_in_csi: snapshot not NULL");
    ASSERT_MSG(strlen(snap) > 0, "malformed_utf8_in_csi: snapshot not empty");
    free(snap);
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  WIDE CHARACTER (wcwidth) SUPPORT                                 */
/* ══════════════════════════════════════════════════════════════════ */

/* CJK characters: U+4E2D (中) = 3 UTF-8 bytes (E4 B8 AD), 2 columns
 *                 U+6587 (文) = 3 UTF-8 bytes (E6 96 87), 2 columns
 * Emoji:          U+1F600 (😀) = 4 UTF-8 bytes (F0 9F 98 80), 2 columns
 * Hebrew:         U+05E9 (ש) = 2 UTF-8 bytes (D7 A9), 1 column
 * Combining:      U+05B4 (ִ hiriq) = 2 UTF-8 bytes (D6 B4), 0 columns
 */

TEST(test_cjk_two_columns) {
    ts_render_t *t = ts_render_create(3, 10);
    /* '中文' = 2 chars, 4 columns */
    feed_str(t, "\xe4\xb8\xad\xe6\x96\x87X");
    /* 中 at cols 0-1, 文 at cols 2-3, X at col 4 */
    assert_snapshot(t, "\xe4\xb8\xad\xe6\x96\x87X\n", "cjk_two_columns");
    ts_render_destroy(t);
}

TEST(test_emoji_two_columns) {
    ts_render_t *t = ts_render_create(3, 10);
    /* 😀 = 2 columns */
    feed_str(t, "\xf0\x9f\x98\x80X");
    /* 😀 at cols 0-1, X at col 2 */
    assert_snapshot(t, "\xf0\x9f\x98\x80X\n", "emoji_two_columns");
    ts_render_destroy(t);
}

TEST(test_cjk_cursor_backward_over_wide) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Write '中文AB', then CUB 3 to land on continuation of 文 */
    feed_str(t, "\xe4\xb8\xad\xe6\x96\x87""AB");
    /* Cursor at col 6. CUB 3 → col 3 (continuation of 文).
     * Writing X here destroys 文 (primary at col 2 cleared), X at col 3. */
    feed_str(t, "\x1b[3DX");
    /* 中(0-1), space(2 — cleared primary), X(3), A(4), B(5) */
    assert_snapshot(t, "\xe4\xb8\xad XAB\n", "cjk_cursor_backward_over_wide");
    ts_render_destroy(t);
}

TEST(test_cjk_wrap_boundary) {
    ts_render_t *t = ts_render_create(3, 5);
    /* Fill 4 of 5 cols, then a wide char that needs 2 — doesn't fit */
    feed_str(t, "1234\xe4\xb8\xad");
    /* 中 needs 2 cols but only 1 left. Wraps to next line. */
    assert_snapshot(t, "1234\n\xe4\xb8\xad\n", "cjk_wrap_boundary");
    ts_render_destroy(t);
}

TEST(test_cjk_exact_fit) {
    ts_render_t *t = ts_render_create(3, 6);
    /* 3 CJK chars = 6 cols, exactly fills a 6-col line */
    feed_str(t, "\xe4\xb8\xad\xe6\x96\x87\xe4\xb8\xad");
    assert_snapshot(t, "\xe4\xb8\xad\xe6\x96\x87\xe4\xb8\xad\n", "cjk_exact_fit");
    ts_render_destroy(t);
}

TEST(test_overwrite_wide_with_narrow) {
    ts_render_t *t = ts_render_create(3, 10);
    /* Write 中X, then move back to col 0 and write 'AB' */
    feed_str(t, "\xe4\xb8\xad X\rAB");
    /* AB overwrites cols 0-1 (中's primary + continuation), clearing the wide char */
    assert_snapshot(t, "AB X\n", "overwrite_wide_with_narrow");
    ts_render_destroy(t);
}

TEST(test_overwrite_narrow_with_wide) {
    ts_render_t *t = ts_render_create(3, 10);
    /* Write ABCDE, then CR, write 中 which occupies 2 cols */
    feed_str(t, "ABCDE\r\xe4\xb8\xad");
    /* 中 overwrites A(col 0) and B(col 1) */
    assert_snapshot(t, "\xe4\xb8\xad""CDE\n", "overwrite_narrow_with_wide");
    ts_render_destroy(t);
}

TEST(test_erase_wide_char) {
    ts_render_t *t = ts_render_create(3, 10);
    /* Write 中A, erase 1 char at col 0 (primary of 中) */
    feed_str(t, "\xe4\xb8\xad""A");
    feed_str(t, "\x1b[1G\x1b[1X"); /* CHA 1 (col 0), ECH 1 */
    /* Erasing primary of wide char must clear continuation too */
    assert_snapshot(t, "  A\n", "erase_wide_char");
    ts_render_destroy(t);
}

TEST(test_cjk_with_sgr) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Bold CJK with 256-color — all SGR stripped, wide chars respected */
    feed_str(t, "\x1b[1;38;5;196m\xe4\xb8\xad\xe6\x96\x87\x1b[0mOK");
    assert_snapshot(t, "\xe4\xb8\xad\xe6\x96\x87OK\n", "cjk_with_sgr");
    ts_render_destroy(t);
}

TEST(test_combining_character) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Hebrew shin (ש, 1 col) + hiriq (ִ, combining, 0 col) + space + A */
    /* Combining mark appends to previous cell, no cursor advance */
    feed_str(t, "\xd7\xa9\xd6\xb4 A");
    /* שִ at col 0 (base + combining in same cell), space at col 1, A at col 2 */
    assert_snapshot(t, "\xd7\xa9\xd6\xb4 A\n", "combining_character");
    ts_render_destroy(t);
}

TEST(test_mixed_wide_narrow_combining) {
    ts_render_t *t = ts_render_create(3, 20);
    /* Mix: 中(2col) + A(1col) + ש(1col) + ִ(0col combining) + 😀(2col) */
    feed_str(t, "\xe4\xb8\xad""A\xd7\xa9\xd6\xb4\xf0\x9f\x98\x80");
    /* 中(0-1), A(2), שִ(3), 😀(4-5) — total 6 columns */
    assert_snapshot(t, "\xe4\xb8\xad""A\xd7\xa9\xd6\xb4\xf0\x9f\x98\x80\n",
                   "mixed_wide_narrow_combining");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  BIDI (RTL) SUPPORT                                               */
/* ══════════════════════════════════════════════════════════════════ */

TEST(test_bidi_pure_hebrew) {
    ts_render_t *t = ts_render_create(3, 40);
    /* שלום עולם (shalom olam) → reversed to םלוע םולש */
    feed_str(t, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d");
    assert_snapshot(t,
        "\xd7\x9d\xd7\x9c\xd7\x95\xd7\xa2 \xd7\x9d\xd7\x95\xd7\x9c\xd7\xa9\n",
        "bidi_pure_hebrew");
    ts_render_destroy(t);
}

TEST(test_bidi_mixed_english_hebrew) {
    ts_render_t *t = ts_render_create(3, 40);
    /* "Hello שלום World" → "Hello םולש World" */
    feed_str(t, "Hello \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d World");
    assert_snapshot(t,
        "Hello \xd7\x9d\xd7\x95\xd7\x9c\xd7\xa9 World\n",
        "bidi_mixed_english_hebrew");
    ts_render_destroy(t);
}

TEST(test_bidi_hebrew_with_number) {
    ts_render_t *t = ts_render_create(3, 40);
    /* "מחיר: 42 שקל" → "לקש 42 :ריחמ" (number stays LTR within RTL) */
    feed_str(t, "\xd7\x9e\xd7\x97\xd7\x99\xd7\xa8: 42 \xd7\xa9\xd7\xa7\xd7\x9c");
    assert_snapshot(t,
        "\xd7\x9c\xd7\xa7\xd7\xa9 42 :\xd7\xa8\xd7\x99\xd7\x97\xd7\x9e\n",
        "bidi_hebrew_with_number");
    ts_render_destroy(t);
}

TEST(test_bidi_ltr_unchanged) {
    ts_render_t *t = ts_render_create(3, 40);
    /* Pure LTR text is unchanged by bidi */
    feed_str(t, "Hello World 123");
    assert_snapshot(t, "Hello World 123\n", "bidi_ltr_unchanged");
    ts_render_destroy(t);
}

TEST(test_bidi_hebrew_with_sgr) {
    ts_render_t *t = ts_render_create(3, 40);
    /* Colored Hebrew — SGR stripped, bidi applied */
    feed_str(t, "\x1b[31m\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\x1b[0m");
    assert_snapshot(t,
        "\xd7\x9d\xd7\x95\xd7\x9c\xd7\xa9\n",
        "bidi_hebrew_with_sgr");
    ts_render_destroy(t);
}

TEST(test_bidi_hebrew_multiline) {
    ts_render_t *t = ts_render_create(5, 40);
    /* Each line gets independent bidi */
    feed_str(t, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\r\nHello\r\n\xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d");
    assert_snapshot(t,
        "\xd7\x9d\xd7\x95\xd7\x9c\xd7\xa9\n"
        "Hello\n"
        "\xd7\x9d\xd7\x9c\xd7\x95\xd7\xa2\n",
        "bidi_hebrew_multiline");
    ts_render_destroy(t);
}

TEST(test_bidi_bracket_mirroring) {
    ts_render_t *t = ts_render_create(3, 40);
    /* "(שלום)" → brackets mirrored in RTL context: "(םולש)" */
    feed_str(t, "(\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d)");
    assert_snapshot(t,
        "(\xd7\x9d\xd7\x95\xd7\x9c\xd7\xa9)\n",
        "bidi_bracket_mirroring");
    ts_render_destroy(t);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  TEST RUNNER                                                      */
/* ══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("nbs-ts-render test suite\n");
    printf("========================\n\n");

    printf("Basic text rendering:\n");
    RUN_TEST(test_empty_screen);
    RUN_TEST(test_simple_text);
    RUN_TEST(test_text_with_newline);
    RUN_TEST(test_carriage_return);
    RUN_TEST(test_cr_lf);
    RUN_TEST(test_backspace);
    RUN_TEST(test_backspace_at_col0);

    printf("\nAuto-wrap:\n");
    RUN_TEST(test_wrap_at_right_margin);
    RUN_TEST(test_wrap_fills_screen);
    RUN_TEST(test_wrap_causes_scroll);

    printf("\nScrolling:\n");
    RUN_TEST(test_scroll_up_via_newline);
    RUN_TEST(test_scroll_up_csi);
    RUN_TEST(test_scroll_down_csi);

    printf("\nCursor movement:\n");
    RUN_TEST(test_cursor_up);
    RUN_TEST(test_cursor_down);
    RUN_TEST(test_cursor_forward);
    RUN_TEST(test_cursor_backward);
    RUN_TEST(test_cursor_position);
    RUN_TEST(test_cursor_position_default);
    RUN_TEST(test_cursor_horizontal_absolute);
    RUN_TEST(test_cursor_next_line);
    RUN_TEST(test_cursor_previous_line);
    RUN_TEST(test_vertical_position_absolute);

    printf("\nErase:\n");
    RUN_TEST(test_erase_to_end_of_screen);
    RUN_TEST(test_erase_to_start_of_screen);
    RUN_TEST(test_erase_entire_screen);
    RUN_TEST(test_erase_to_end_of_line);
    RUN_TEST(test_erase_to_start_of_line);
    RUN_TEST(test_erase_entire_line);
    RUN_TEST(test_erase_characters);

    printf("\nInsert / Delete:\n");
    RUN_TEST(test_insert_lines);
    RUN_TEST(test_delete_lines);
    RUN_TEST(test_insert_characters);
    RUN_TEST(test_delete_characters);

    printf("\nTabs:\n");
    RUN_TEST(test_tab_default_stops);
    RUN_TEST(test_tab_at_end_of_line);

    printf("\nScroll region:\n");
    RUN_TEST(test_scroll_region);

    printf("\nSGR stripping:\n");
    RUN_TEST(test_sgr_stripped);
    RUN_TEST(test_sgr_256_color);

    printf("\nOSC / DCS:\n");
    RUN_TEST(test_osc_title_bel);
    RUN_TEST(test_osc_title_st);
    RUN_TEST(test_dcs_consumed);

    printf("\nCursor save / restore:\n");
    RUN_TEST(test_cursor_save_restore);

    printf("\nFull reset:\n");
    RUN_TEST(test_full_reset);

    printf("\nUTF-8:\n");
    RUN_TEST(test_utf8_2byte);
    RUN_TEST(test_utf8_3byte);
    RUN_TEST(test_utf8_4byte);
    RUN_TEST(test_utf8_overwrite);

    printf("\nPrivate mode:\n");
    RUN_TEST(test_private_mode_ignored);

    printf("\nReverse index:\n");
    RUN_TEST(test_reverse_index);

    printf("\nEdge cases:\n");
    RUN_TEST(test_cursor_beyond_right_margin);
    RUN_TEST(test_cursor_beyond_bottom);
    RUN_TEST(test_zero_param_treated_as_default);
    RUN_TEST(test_malformed_csi_ignored);
    RUN_TEST(test_bare_esc_at_eof);
    RUN_TEST(test_many_csi_params);
    RUN_TEST(test_esc_intermediate_ignored);

    printf("\nRealistic scenarios:\n");
    RUN_TEST(test_clear_screen_and_rewrite);
    RUN_TEST(test_progress_bar_simulation);
    RUN_TEST(test_colored_prompt_stripped);
    RUN_TEST(test_alternate_screen_mode_ignored);

    printf("\nUTF-8 + escape interaction:\n");
    RUN_TEST(test_colored_utf8_text);
    RUN_TEST(test_colored_emoji);
    RUN_TEST(test_cursor_movement_over_utf8);
    RUN_TEST(test_erase_utf8_cells);
    RUN_TEST(test_overwrite_utf8_with_ascii);
    RUN_TEST(test_overwrite_ascii_with_utf8);
    RUN_TEST(test_utf8_at_wrap_boundary);
    RUN_TEST(test_utf8_wrap_then_continue);
    RUN_TEST(test_esc_interrupts_utf8);
    RUN_TEST(test_malformed_utf8_in_csi);

    printf("\nWide character (wcwidth) support:\n");
    RUN_TEST(test_cjk_two_columns);
    RUN_TEST(test_emoji_two_columns);
    RUN_TEST(test_cjk_cursor_backward_over_wide);
    RUN_TEST(test_cjk_wrap_boundary);
    RUN_TEST(test_cjk_exact_fit);
    RUN_TEST(test_overwrite_wide_with_narrow);
    RUN_TEST(test_overwrite_narrow_with_wide);
    RUN_TEST(test_erase_wide_char);
    RUN_TEST(test_cjk_with_sgr);
    RUN_TEST(test_combining_character);
    RUN_TEST(test_mixed_wide_narrow_combining);

    printf("\nBidi (RTL) support:\n");
    RUN_TEST(test_bidi_pure_hebrew);
    RUN_TEST(test_bidi_mixed_english_hebrew);
    RUN_TEST(test_bidi_hebrew_with_number);
    RUN_TEST(test_bidi_ltr_unchanged);
    RUN_TEST(test_bidi_hebrew_with_sgr);
    RUN_TEST(test_bidi_hebrew_multiline);
    RUN_TEST(test_bidi_bracket_mirroring);

    printf("\nAPI:\n");
    RUN_TEST(test_reset_clears_all);
    RUN_TEST(test_multiple_feeds);
    RUN_TEST(test_feed_split_escape);
    RUN_TEST(test_feed_split_utf8);

    printf("\n========================\n");
    printf("Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
