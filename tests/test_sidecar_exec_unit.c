/*
 * test_sidecar_exec_unit.c — Unit tests for exec_util.
 *
 * Falsifiable claims tested:
 *   1. exec_capture: "echo hello" captures "hello\n", returns 0
 *   2. exec_capture: multi-line output is captured correctly
 *   3. exec_capture: nonexistent command returns 127
 *   4. exec_capture: buffer too small truncates without crash
 *   5. exec_fire_and_forget: "true" returns 0
 *   6. exec_fire_and_forget: "false" returns 1
 *   7. exec_fire_and_forget: nonexistent command returns 127 or -1
 */

#include "../src/nbs-sidecar/exec_util.h"
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
    printf("test_sidecar_exec_unit\n");

    /* 1. exec_capture: echo hello → "hello\n", return 0 */
    {
        char buf[256];
        const char *argv[] = {"echo", "hello", NULL};
        int rc = exec_capture(argv, buf, sizeof(buf));
        CHECK("echo hello returns 0", rc == 0);
        CHECK("echo hello captures \"hello\\n\"",
              strcmp(buf, "hello\n") == 0);
    }

    /* 2. exec_capture: multi-line output */
    {
        char buf[256];
        const char *argv[] = {"printf", "line1\nline2\n", NULL};
        int rc = exec_capture(argv, buf, sizeof(buf));
        CHECK("printf multi-line returns 0", rc == 0);
        CHECK("printf multi-line captures both lines",
              strcmp(buf, "line1\nline2\n") == 0);
    }

    /* 3. exec_capture: nonexistent command returns 127 */
    {
        char buf[256];
        const char *argv[] = {"__nbs_no_such_command_xyz__", NULL};
        int rc = exec_capture(argv, buf, sizeof(buf));
        CHECK("nonexistent command returns 127", rc == 127);
    }

    /* 4. exec_capture: buffer too small truncates, doesn't crash */
    {
        char buf[4]; /* only room for 3 chars + NUL */
        const char *argv[] = {"echo", "hello", NULL};
        int rc = exec_capture(argv, buf, sizeof(buf));
        CHECK("small buffer returns 0", rc == 0);
        CHECK("small buffer truncates to 3 chars",
              strlen(buf) == 3 && strncmp(buf, "hel", 3) == 0);
        CHECK("small buffer is NUL-terminated", buf[3] == '\0');
    }

    /* 5. exec_fire_and_forget: true returns 0 */
    {
        const char *argv[] = {"true", NULL};
        int rc = exec_fire_and_forget(argv);
        CHECK("true returns 0", rc == 0);
    }

    /* 6. exec_fire_and_forget: false returns 1 */
    {
        const char *argv[] = {"false", NULL};
        int rc = exec_fire_and_forget(argv);
        CHECK("false returns 1", rc == 1);
    }

    /* 7. exec_fire_and_forget: nonexistent returns 127 or -1 */
    {
        const char *argv[] = {"__nbs_no_such_command_xyz__", NULL};
        int rc = exec_fire_and_forget(argv);
        CHECK("nonexistent fire_and_forget returns 127 or -1",
              rc == 127 || rc == -1);
    }

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
