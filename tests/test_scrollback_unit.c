/*
 * test_scrollback_unit.c — Unit tests for scrollback screen line splitting.
 *
 * Tests the logic that splits rendered message output into screen lines,
 * accounting for terminal wrapping and ANSI escape sequences.
 *
 * Build:
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -o test_scrollback_unit test_scrollback_unit.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("   PASS: %s\n", label); g_pass++; } \
    else { printf("   FAIL: %s (line %d)\n", label, __LINE__); g_fail++; } \
} while(0)

/* --- Copy of visible_line_width from terminal.c --- */

static int visible_line_width(const char **pp) {
    int w = 0;
    const char *p = *pp;
    while (*p && *p != '\n') {
        if (*p == '\033') {
            p++;
            if (*p == '[') {
                p++;
                while (*p && !((*p >= 'A' && *p <= 'Z') ||
                               (*p >= 'a' && *p <= 'z')))
                    p++;
                if (*p) p++;
            }
            continue;
        }
        w++;
        p++;
    }
    if (*p == '\n') p++;
    *pp = p;
    return w;
}

/* --- Copy of split_screen_lines from terminal.c --- */

#define SCROLLBACK_MAX_LINES 131072

static char **split_screen_lines(char *buf, int term_width,
                                 int *out_count) {
    char **lines = calloc(SCROLLBACK_MAX_LINES, sizeof(char *));
    if (!lines) { *out_count = 0; return NULL; }
    int count = 0;

    char *p = buf;
    while (*p && count < SCROLLBACK_MAX_LINES) {
        char *line_start = p;
        const char *scan = p;
        int vis_width = visible_line_width(&scan);

        if (vis_width <= term_width || term_width <= 0) {
            lines[count++] = line_start;
            p = (char *)scan;
        } else {
            const char *cp = line_start;
            const char *nl = cp;
            while (*nl && *nl != '\n') nl++;

            while (cp < nl && count < SCROLLBACK_MAX_LINES) {
                lines[count++] = (char *)cp;
                int col = 0;
                while (cp < nl) {
                    if (*cp == '\033') {
                        cp++;
                        if (*cp == '[') {
                            cp++;
                            while (*cp && !((*cp >= 'A' && *cp <= 'Z')
                                   || (*cp >= 'a' && *cp <= 'z')))
                                cp++;
                            if (*cp) cp++;
                        }
                        continue;
                    }
                    col++;
                    cp++;
                    if (col >= term_width) break;
                }
            }
            if (*nl == '\n') nl++;
            p = (char *)nl;
        }
    }
    *out_count = count;
    return lines;
}

/* --- Tests --- */

static void test_simple_lines(void) {
    printf("1. Simple lines (no wrapping)...\n");
    char buf[] = "hello\nworld\n";
    int count = 0;
    char **lines = split_screen_lines(buf, 80, &count);
    CHECK("two lines", count == 2);
    CHECK("first line starts at 'hello'", strncmp(lines[0], "hello", 5) == 0);
    CHECK("second line starts at 'world'", strncmp(lines[1], "world", 5) == 0);
    free(lines);
}

static void test_wrapping(void) {
    printf("2. Line wrapping at terminal width...\n");
    /* 20 chars wide, line is 35 visible chars -> should be 2 screen lines */
    char buf[] = "12345678901234567890123456789012345\n";
    int count = 0;
    char **lines = split_screen_lines(buf, 20, &count);
    CHECK("wrapped to 2 lines", count == 2);
    CHECK("first starts at beginning", lines[0] == buf);
    free(lines);
}

static void test_exact_width(void) {
    printf("3. Line exactly terminal width...\n");
    char buf[] = "12345678901234567890\n";  /* exactly 20 */
    int count = 0;
    char **lines = split_screen_lines(buf, 20, &count);
    CHECK("exact width = 1 line", count == 1);
    free(lines);
}

static void test_ansi_not_counted(void) {
    printf("4. ANSI escapes not counted as width...\n");
    /* 5 visible chars + ANSI codes = should fit in 10 columns */
    char buf[] = "\033[1mhello\033[0m\n";
    int count = 0;
    char **lines = split_screen_lines(buf, 10, &count);
    CHECK("ANSI line = 1 screen line", count == 1);
    free(lines);
}

static void test_ansi_wrapping(void) {
    printf("5. Long line with ANSI wraps correctly...\n");
    /* 30 visible chars with ANSI in the middle, width=20 -> 2 lines */
    char buf[] = "1234567890\033[1m1234567890\033[0m1234567890\n";
    int count = 0;
    char **lines = split_screen_lines(buf, 20, &count);
    CHECK("ANSI wrapped to 2 lines", count == 2);
    free(lines);
}

static void test_empty_line(void) {
    printf("6. Empty line...\n");
    char buf[] = "\n";
    int count = 0;
    char **lines = split_screen_lines(buf, 80, &count);
    CHECK("empty line = 1 screen line", count == 1);
    free(lines);
}

static void test_multiple_wraps(void) {
    printf("7. Line wrapping multiple times...\n");
    /* 50 chars at width 10 -> 5 screen lines */
    char buf[] = "12345678901234567890123456789012345678901234567890\n";
    int count = 0;
    char **lines = split_screen_lines(buf, 10, &count);
    CHECK("50 chars at width 10 = 5 lines", count == 5);
    free(lines);
}

static void test_mixed(void) {
    printf("8. Mix of short and long lines...\n");
    char buf[] = "short\n"
                 "this is a longer line that wraps at twenty\n"
                 "tiny\n";
    int count = 0;
    char **lines = split_screen_lines(buf, 20, &count);
    /* "short" = 1, "this is a longer..." = 3 (43 chars / 20), "tiny" = 1 */
    CHECK("mixed = 5 screen lines", count == 5);
    free(lines);
}

static void test_no_trailing_newline(void) {
    printf("9. Content without trailing newline...\n");
    char buf[] = "hello";
    int count = 0;
    char **lines = split_screen_lines(buf, 80, &count);
    CHECK("no trailing newline = 1 line", count == 1);
    free(lines);
}

int main(void) {
    printf("=== Scrollback Screen Line Split Tests ===\n\n");

    test_simple_lines();
    test_wrapping();
    test_exact_width();
    test_ansi_not_counted();
    test_ansi_wrapping();
    test_empty_line();
    test_multiple_wraps();
    test_mixed();
    test_no_trailing_newline();

    printf("\n=== Result ===\n");
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
