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
 *  13. DEL character (0x7F) is stripped (BUG fix).
 *  14. DEL mixed with text and escapes is stripped (BUG fix).
 *  15. C1 control codes (0x80-0x9F) are stripped (HARDENING).
 *  16. C1 codes vs byte above C1 range (0xA0+) passes through.
 *  17. Bare control chars (CR, BEL, NUL-adjacent) stripped, newlines/tabs kept.
 *  18. UTF-8 multibyte characters pass through unchanged.
 *  19. Postcondition: output length <= input length for all-escape input.
 *  20. String of only control characters produces empty output.
 *  21. B16 adversarial: U+0100 (Ā, 0xC4 0x80) passes through unchanged.
 *  22. B16 adversarial: standalone C1 control 0x85 is still stripped.
 *  23. B16 adversarial: U+0080 (0xC2 0x80) — valid UTF-8 2-byte seq preserved.
 *  24. B16 adversarial: mixed UTF-8 multi-byte and standalone C1 controls.
 *  25. B16 adversarial: 3-byte UTF-8 (U+4E16, 0xE4 0xB8 0x96) preserved.
 *  26. B16 adversarial: 4-byte UTF-8 (U+1F600, 0xF0 0x9F 0x98 0x80) preserved.
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

    /* --- Adversarial tests for BUG fixes --- */

    /* 13. DEL character (0x7F) is stripped.
     * DEL is a control character that appears in terminal captures
     * (backspace/delete key sequences). Before the fix, it passed through. */
    {
        char buf[] = "hello\x7Fworld";
        size_t len = strip_ansi(buf);
        CHECK("DEL stripped", strcmp(buf, "helloworld") == 0);
        CHECK("DEL stripped length", len == 10);
    }

    /* 14. DEL mixed with escapes and text */
    {
        char buf[] = "\x1b[31m\x7Ftext\x7F\x1b[0m\x7F";
        size_t len = strip_ansi(buf);
        CHECK("DEL+escapes stripped", strcmp(buf, "text") == 0);
        CHECK("DEL+escapes length", len == 4);
    }

    /* 15. C1 control codes (0x80-0x9F) are stripped.
     * These are 8-bit control characters that tmux applications may emit.
     * 0x9B is 8-bit CSI, 0x9D is 8-bit OSC — both should be stripped. */
    {
        /* 0x90 (DCS), 0x9B (CSI), 0x9D (OSC) — all C1 controls */
        char buf[] = "abc\x90\x9B\x9D" "def";
        size_t len = strip_ansi(buf);
        CHECK("C1 controls stripped", strcmp(buf, "abcdef") == 0);
        CHECK("C1 controls length", len == 6);
    }

    /* 16. Byte 0xA0 (first byte above C1 range) passes through */
    {
        char buf[] = "a\xA0" "b";
        size_t len = strip_ansi(buf);
        CHECK("0xA0 passes through", len == 3);
        CHECK("0xA0 content", buf[0] == 'a' && (unsigned char)buf[1] == 0xA0 && buf[2] == 'b');
    }

    /* 17. Bare control chars stripped, newlines and tabs kept */
    {
        /* CR (0x0D), BEL (0x07), form feed (0x0C) stripped; \n and \t kept */
        char buf[] = "line1\r\n\tindented\x07\x0C" "end";
        size_t len = strip_ansi(buf);
        CHECK("control chars stripped, nl/tab kept",
              strcmp(buf, "line1\n\tindentedend") == 0);
        CHECK("control chars length", len == strlen("line1\n\tindentedend"));
    }

    /* 18. UTF-8 multibyte characters (>= 0xA0) pass through unchanged */
    {
        /* UTF-8 for U+00E9 (e-acute) = 0xC3 0xA9 */
        char buf[] = "caf\xC3\xA9";
        size_t len = strip_ansi(buf);
        CHECK("UTF-8 multibyte unchanged", strcmp(buf, "caf\xC3\xA9") == 0);
        CHECK("UTF-8 multibyte length", len == 5);
    }

    /* 19. All-escape input produces empty output (postcondition stress test) */
    {
        char buf[] = "\x1b[1m\x1b[31m\x1b]0;t\x07\x1bM\x1b";
        size_t len = strip_ansi(buf);
        CHECK("all-escape produces empty", strcmp(buf, "") == 0);
        CHECK("all-escape length 0", len == 0);
    }

    /* 20. String of only control characters produces empty output */
    {
        char buf[] = "\x01\x02\x03\x04\x05\x06\x07\x0B\x0C\x0D\x0E\x0F"
                     "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1A\x1C"
                     "\x1D\x1E\x1F\x7F";
        size_t len = strip_ansi(buf);
        CHECK("all control chars stripped", len == 0);
        CHECK("all control chars empty", buf[0] == '\0');
    }

    /* --- Adversarial tests for B16: UTF-8 vs C1 control stripping --- */

    /* 21. U+0100 (Ā) = 0xC4 0x80 — must pass through unchanged.
     * This is the primary adversarial test for B16. The continuation byte
     * 0x80 falls in the C1 range but must NOT be stripped when it follows
     * a valid UTF-8 leading byte. */
    {
        char buf[] = "Hello \xC4\x80 world";
        size_t len = strip_ansi(buf);
        CHECK("B16: U+0100 unchanged", strcmp(buf, "Hello \xC4\x80 world") == 0);
        CHECK("B16: U+0100 length", len == strlen("Hello \xC4\x80 world"));
    }

    /* 22. Standalone C1 control 0x85 (NEL) is still stripped.
     * Without a preceding UTF-8 leading byte, 0x85 is a C1 control. */
    {
        char buf[] = "before\x85" "after";
        size_t len = strip_ansi(buf);
        CHECK("B16: standalone 0x85 stripped", strcmp(buf, "beforeafter") == 0);
        CHECK("B16: standalone 0x85 length", len == 11);
    }

    /* 23. U+0080 = 0xC2 0x80 — valid 2-byte UTF-8 sequence preserved.
     * 0xC2 is a valid leading byte; 0x80 is the continuation byte. */
    {
        char buf[] = "x\xC2\x80y";
        size_t len = strip_ansi(buf);
        CHECK("B16: U+0080 (0xC2 0x80) preserved", len == 4);
        CHECK("B16: U+0080 content",
              buf[0] == 'x' && (unsigned char)buf[1] == 0xC2 &&
              (unsigned char)buf[2] == 0x80 && buf[3] == 'y');
    }

    /* 24. Mixed: valid UTF-8 multi-byte + standalone C1 controls.
     * U+0100 (0xC4 0x80) should be preserved, standalone 0x90 stripped. */
    {
        char buf[] = "\xC4\x80\x90\xC4\x80";
        size_t len = strip_ansi(buf);
        CHECK("B16: mixed UTF-8 and C1",
              len == 4 && (unsigned char)buf[0] == 0xC4 &&
              (unsigned char)buf[1] == 0x80 && (unsigned char)buf[2] == 0xC4 &&
              (unsigned char)buf[3] == 0x80);
    }

    /* 25. 3-byte UTF-8: U+4E16 (世) = 0xE4 0xB8 0x96.
     * Continuation bytes 0xB8 and 0x96 are both in 0x80-0xBF range;
     * 0x96 is in C1 range. Must be preserved. */
    {
        char buf[] = "\xE4\xB8\x96";
        size_t len = strip_ansi(buf);
        CHECK("B16: 3-byte UTF-8 (世) preserved", len == 3);
        CHECK("B16: 3-byte content",
              (unsigned char)buf[0] == 0xE4 && (unsigned char)buf[1] == 0xB8 &&
              (unsigned char)buf[2] == 0x96);
    }

    /* 26. 4-byte UTF-8: U+1F600 (😀) = 0xF0 0x9F 0x98 0x80.
     * All three continuation bytes (0x9F, 0x98, 0x80) are in C1 range.
     * This is the worst case — all continuations would be stripped by
     * the old code. */
    {
        char buf[] = "\xF0\x9F\x98\x80";
        size_t len = strip_ansi(buf);
        CHECK("B16: 4-byte UTF-8 (U+1F600) preserved", len == 4);
        CHECK("B16: 4-byte content",
              (unsigned char)buf[0] == 0xF0 && (unsigned char)buf[1] == 0x9F &&
              (unsigned char)buf[2] == 0x98 && (unsigned char)buf[3] == 0x80);
    }

    /* --- Tests for cursor-right expansion (CSI n C → spaces) --- */

    /* 27. Small cursor-right: CSI 3 C → 3 spaces.
     * Sequence is ESC [ 3 C = 4 bytes. Gap is large. Should produce 3 spaces. */
    {
        char buf[] = "ab\x1b[3Cde";
        size_t len = strip_ansi(buf);
        CHECK("cursor right 3 spaces", strcmp(buf, "ab   de") == 0);
        CHECK("cursor right 3 length", len == 7);
    }

    /* 28. Cursor-right with no param: CSI C → 1 space.
     * ESC [ C = 3 bytes. Should produce 1 space. */
    {
        char buf[] = "x\x1b[Cy";
        size_t len = strip_ansi(buf);
        CHECK("cursor right default 1 space", strcmp(buf, "x y") == 0);
        CHECK("cursor right default length", len == 3);
    }

    /* 29. Large cursor-right capped to 8: CSI 20 C → 8 spaces max.
     * ESC [ 2 0 C = 5 bytes. Cap is 8 but gap also limits. */
    {
        char buf[] = "\x1b[20Cend";
        size_t len = strip_ansi(buf);
        /* Gap between wr (start) and rd (after 'C') is 5 bytes.
         * 20 spaces requested, capped to gap (5), then capped to 8.
         * So 5 spaces + "end" */
        CHECK("cursor right capped to gap", len == 8);
        CHECK("cursor right capped content ends with 'end'",
              strcmp(buf + len - 3, "end") == 0);
    }

    /* 30. Cursor-right invariant: wr never overtakes rd.
     * This was the original bug — CSI 8 C at start of string
     * would write 8 spaces from 4 bytes of input. */
    {
        char buf[] = "\x1b[8Cx";
        size_t len = strip_ansi(buf);
        /* ESC [ 8 C = 4 bytes, then 'x'. Gap at start is 4.
         * 8 spaces requested, capped to 4 (gap). Then 'x' copied. */
        CHECK("cursor right 8 at start: no overflow", len == 5);
        CHECK("cursor right 8 at start: ends with x",
              buf[len - 1] == 'x');
        /* Verify no assertion fired (test reaching here = pass) */
        CHECK("cursor right 8 at start: invariant held", 1);
    }

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
