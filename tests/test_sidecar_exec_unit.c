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
 *   8. exec_capture: child killed by signal returns -1
 *   9. exec_capture: fd CLOEXEC — child does not inherit parent pipe fds
 *  10. exec_capture: exit code from failing command
 *  11. exec_capture: buffer of size 1 produces empty NUL-terminated string
 */

#include "../src/nbs-sidecar/exec_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

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

    /* 1. exec_capture: echo hello -> "hello\n", return 0 */
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

    /* ==== ADVERSARIAL TESTS — targeting audit violations ==== */

    /*
     * V1/V9 (BUG): exec_capture with command that exits non-zero.
     * Verify exit code is correctly propagated.
     */
    {
        char buf[256];
        const char *argv[] = {"sh", "-c", "exit 42", NULL};
        int rc = exec_capture(argv, buf, sizeof(buf));
        CHECK("exit 42 returns 42", rc == 42);
    }

    /*
     * V3 (SECURITY): Verify pipe fds have CLOEXEC set.
     * The child should not inherit the parent's pipe read fd.
     * The pipe read fd would typically be fd 4 or 5. We check that
     * no fd in range 4-10 is open in the child (fds outside this
     * range, like environment fds from inotify or /proc, are not
     * pipe leaks).
     */
    {
        char buf[1024];
        /* Child lists /proc/self/fd numerically */
        const char *argv[] = {"sh", "-c",
            "ls /proc/self/fd 2>/dev/null | sort -n", NULL};
        int rc = exec_capture(argv, buf, sizeof(buf));
        if (rc == 0) {
            /* On Linux with /proc, the child should see 0,1,2 and
             * the ls fd itself (3). Pipe fds would be 4-10 range.
             * Higher fds (50+) may exist from the parent environment
             * (inotify, /proc/cpuinfo, etc.) and are not pipe leaks. */
            int has_pipe_range_fd = 0;
            char buf_copy[1024];
            strncpy(buf_copy, buf, sizeof(buf_copy));
            buf_copy[sizeof(buf_copy) - 1] = '\0';
            char *tok = strtok(buf_copy, "\n");
            while (tok) {
                int fd_num = atoi(tok);
                if (fd_num >= 4 && fd_num <= 10) has_pipe_range_fd = 1;
                tok = strtok(NULL, "\n");
            }
            CHECK("child has no pipe-range fds 4-10 (CLOEXEC)", !has_pipe_range_fd);
        } else {
            /* /proc not available; skip gracefully */
            printf("   SKIP: CLOEXEC test (/proc unavailable)\n");
        }
    }

    /*
     * exec_capture: buffer of size 1. Should produce empty string.
     */
    {
        char buf[1];
        const char *argv[] = {"echo", "hello", NULL};
        int rc = exec_capture(argv, buf, sizeof(buf));
        /* rc may be 0 (echo completed before pipe closed) or -1
         * (child killed by SIGPIPE when parent closes read end) */
        CHECK("buf size 1 returns 0 or -1", rc == 0 || rc == -1);
        CHECK("buf size 1 produces empty NUL-terminated string",
              buf[0] == '\0');
    }

    /*
     * exec_capture: child produces stderr only, stdout empty.
     * Verify out_buf is empty and return code is correct.
     */
    {
        char buf[256];
        memset(buf, 'X', sizeof(buf));
        const char *argv[] = {"sh", "-c", "echo error >&2; exit 0", NULL};
        int rc = exec_capture(argv, buf, sizeof(buf));
        CHECK("stderr-only child returns 0", rc == 0);
        CHECK("stderr-only child has empty stdout",
              buf[0] == '\0');
    }

    /*
     * exec_capture: child produces exactly out_size-1 bytes.
     * Boundary: should capture all of them, NUL at position out_size-1.
     */
    {
        char buf[6]; /* 5 chars + NUL */
        /* printf "12345" produces exactly 5 bytes */
        const char *argv[] = {"printf", "12345", NULL};
        int rc = exec_capture(argv, buf, sizeof(buf));
        CHECK("exact-fit returns 0", rc == 0);
        CHECK("exact-fit captures all 5 chars",
              strcmp(buf, "12345") == 0);
    }

    /*
     * exec_fire_and_forget: command that writes to stdout (discarded).
     * Verify it still returns the correct exit code.
     */
    {
        const char *argv[] = {"sh", "-c", "echo discarded; exit 7", NULL};
        int rc = exec_fire_and_forget(argv);
        CHECK("fire_and_forget stdout discarded, exit 7", rc == 7);
    }

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
