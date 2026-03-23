/*
 * assert_check.c — Verify ASSERT_MSG compiles and fires correctly.
 *
 * Standard pattern across all NBS components.
 */

#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <setjmp.h>

static sigjmp_buf jump_buf;
static volatile int abort_caught = 0;

static void handle_abort(int sig)
{
    (void)sig;
    abort_caught = 1;
    siglongjmp(jump_buf, 1);
}

int main(void)
{
    /* Test 1: ASSERT_MSG with true condition should not fire */
    ASSERT_MSG(1 == 1, "This should not fire");
    printf("PASS: true condition did not fire\n");

    /* Test 2: ASSERT_MSG with false condition should abort */
    struct sigaction sa;
    sa.sa_handler = handle_abort;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGABRT, &sa, NULL);

    if (sigsetjmp(jump_buf, 1) == 0) {
        ASSERT_MSG(1 == 0, "Expected abort for test: %d", 42);
        printf("FAIL: false condition did not abort\n");
        return 1;
    }

    if (abort_caught) {
        printf("PASS: false condition aborted correctly\n");
    } else {
        printf("FAIL: unexpected jump\n");
        return 1;
    }

    printf("All assert checks passed\n");
    return 0;
}
