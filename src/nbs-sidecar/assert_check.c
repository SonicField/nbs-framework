/*
 * assert_check.c — Verify NDEBUG is not defined and ASSERT_MSG aborts.
 *
 * ASSERT_MSG must always fire. If NDEBUG is defined, the build is wrong.
 * This binary is run as part of `make test` to catch misconfigured builds.
 *
 * Modes:
 *   (no args)      — verify truthy assertion passes, report OK.
 *   --test-abort   — trigger ASSERT_MSG(0, ...) so parent can verify SIGABRT.
 */

#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * run_abort_subprocess — fork and exec self with --test-abort,
 * verify the child dies from SIGABRT.
 * Returns 0 on success (child aborted as expected), 1 on failure.
 */
static int run_abort_subprocess(const char *self_path) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("assert_check: fork failed");
        return 1;
    }
    if (pid == 0) {
        /* Child: exec self with --test-abort.
         * Redirect stderr to /dev/null to suppress the expected assert message. */
        freopen("/dev/null", "w", stderr);
        execl(self_path, self_path, "--test-abort", (char *)NULL);
        _exit(127); /* exec failed */
    }
    /* Parent: wait and check signal. */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("assert_check: waitpid failed");
        return 1;
    }
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
        return 0; /* Child aborted as expected */
    }
    fprintf(stderr,
            "assert_check: FAIL — expected child SIGABRT, "
            "got exit=%d signal=%d\n",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return 1;
}

int main(int argc, char *argv[]) {
#ifdef NDEBUG
    /* If NDEBUG is defined, ASSERT_MSG may be silently disabled.
     * This is a build system error, not a code error. */
    fprintf(stderr, "FATAL: NDEBUG is defined — assertions are disabled\n");
    return 1;
#endif

    /* --test-abort mode: trigger a false assertion and abort.
     * This is called by the subprocess test, not directly by users. */
    if (argc == 2 && strcmp(argv[1], "--test-abort") == 0) {
        ASSERT_MSG(0, "assert_check --test-abort: deliberate false assertion "
                       "to verify abort fires (expected=abort, actual=reached)");
        /* If we reach here, ASSERT_MSG is broken. */
        fprintf(stderr, "assert_check: FAIL — ASSERT_MSG(0, ...) did not abort\n");
        return 1;
    }

    /* Test 1: truthy assertion must not abort. */
    ASSERT_MSG(1 == 1,
               "assert_check: truthy assertion failed "
               "(expected=no abort, actual=abort, why=ASSERT_MSG fires on true)");
    fprintf(stderr, "assert_check: PASS — truthy assertion did not abort\n");

    /* Test 2: false assertion must abort (verified via subprocess). */
    if (run_abort_subprocess(argv[0]) != 0) {
        fprintf(stderr,
                "assert_check: FAIL — false assertion did not produce SIGABRT\n");
        return 1;
    }
    fprintf(stderr, "assert_check: PASS — false assertion produced SIGABRT\n");

    fprintf(stderr, "assert_check: OK — all checks passed\n");
    return 0;
}
