/*
 * test_sidecar_strip_ansi_unit.c — Unit tests for ANSI escape stripping.
 *
 * Falsifiable claims tested:
 *   1. Plain text passes through unchanged.
 *   2. CSI sequence (e.g. SGR colour) is removed entirely.
 *   3. Multiple CSI sequences in one string are all removed.
 *   4. OSC sequence terminated by BEL is removed.
 *   5. OSC sequence terminated by ST (ESC \) is removed.
 *   6. Simple escape (ESC + single char) is removed.
 *   7. Bare ESC at end of string is removed.
 *   8. Mixed content: escapes removed, text preserved.
 *   9. Empty string returns length 0.
 *  10. Return value equals strlen of result.
 *  11. CSI with parameters (e.g. ESC[38;5;196m) is removed.
 *  12. Adjacent escapes with no text between them are all removed.
 */

#include "../src/nbs-sidecar/strip_ansi.h"
#include <stdio.h>
#include <string.h>

static int tests = 0, fails = 0;

#define CHECK(label, cond) do { \
    tests++; \
    if (!(cond)) { \
        fails++; \
        printf("   FAIL: %s\n", label); \
    } else { \
        printf("   PASS: %s\n", label); \
    } \
} while(0)

int main(void) {
    printf("test_sidecar_strip_ansi_unit\n");

    /* 1. Plain text passes through unchanged */
    {
        char buf[] = "hello world";
        size_t len = strip_ansi(buf);
        CHECK("plain text unchanged", strcmp(buf, "hello world") == 0);
        CHECK("plain text length", len == 11);
    }

    /* 2. CSI sequence (SGR colour) is removed */
    {
        /* ESC[31m = red foreground */
        char buf[] = "\x1b[31mred text\x1b[0m";
        size_t len = strip_ansi(buf);
        CHECK("CSI SGR removed", strcmp(buf, "red text") == 0);
        CHECK("CSI SGR length", len == 8);
    }

    /* 3. Multiple CSI sequences all removed */
    {
        char buf[] = "\x1b[1m\x1b[32mgreen bold\x1b[0m";
        size_t len = strip_ansi(buf);
        CHECK("multiple CSI removed", strcmp(buf, "green bold") == 0);
        CHECK("multiple CSI length", len == 10);
    }

    /* 4. OSC terminated by BEL */
    {
        /* ESC ] 0 ; title BEL — sets terminal title */
        char buf[] = "\x1b]0;my title\x07rest";
        size_t len = strip_ansi(buf);
        CHECK("OSC+BEL removed", strcmp(buf, "rest") == 0);
        CHECK("OSC+BEL length", len == 4);
    }

    /* 5. OSC terminated by ST (ESC \) */
    {
        char buf[] = "\x1b]0;my title\x1b\\rest";
        size_t len = strip_ansi(buf);
        CHECK("OSC+ST removed", strcmp(buf, "rest") == 0);
        CHECK("OSC+ST length", len == 4);
    }

    /* 6. Simple escape (ESC + single char) */
    {
        /* ESC M = reverse index */
        char buf[] = "before\x1bMafter";
        size_t len = strip_ansi(buf);
        CHECK("simple escape removed", strcmp(buf, "beforeafter") == 0);
        CHECK("simple escape length", len == 11);
    }

    /* 7. Bare ESC at end of string */
    {
        char buf[] = "text\x1b";
        size_t len = strip_ansi(buf);
        CHECK("bare ESC at end removed", strcmp(buf, "text") == 0);
        CHECK("bare ESC at end length", len == 4);
    }

    /* 8. Mixed content: escapes removed, text preserved */
    {
        char buf[] = "Hello \x1b[1mworld\x1b[0m!\x1b]0;title\x07 Done\x1bM.";
        size_t len = strip_ansi(buf);
        CHECK("mixed content text preserved",
              strcmp(buf, "Hello world! Done.") == 0);
        CHECK("mixed content length", len == 18);
    }

    /* 9. Empty string returns length 0 */
    {
        char buf[] = "";
        size_t len = strip_ansi(buf);
        CHECK("empty string length 0", len == 0);
        CHECK("empty string still empty", buf[0] == '\0');
    }

    /* 10. Return value equals strlen of result */
    {
        char buf[] = "\x1b[31mtest\x1b[0m string";
        size_t len = strip_ansi(buf);
        CHECK("return value matches strlen", len == strlen(buf));
    }

    /* 11. CSI with extended parameters (256-colour) */
    {
        /* ESC[38;5;196m = 256-colour red foreground */
        char buf[] = "\x1b[38;5;196mcoloured\x1b[0m";
        size_t len = strip_ansi(buf);
        CHECK("CSI 256-colour removed", strcmp(buf, "coloured") == 0);
        CHECK("CSI 256-colour length", len == 8);
    }

    /* 12. Adjacent escapes with no text between them */
    {
        char buf[] = "\x1b[1m\x1b[31m\x1b[4m";
        size_t len = strip_ansi(buf);
        CHECK("adjacent escapes all removed", strcmp(buf, "") == 0);
        CHECK("adjacent escapes length 0", len == 0);
    }

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
