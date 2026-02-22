/*
 * assert_check.c — Verify NDEBUG is not defined.
 *
 * ASSERT_MSG must always fire. If NDEBUG is defined, the build is wrong.
 * This binary is run as part of `make test` to catch misconfigured builds.
 */

#include "../nbs-common/nbs_assert.h"

int main(void) {
#ifdef NDEBUG
    /* If NDEBUG is defined, ASSERT_MSG may be silently disabled.
     * This is a build system error, not a code error. */
    fprintf(stderr, "FATAL: NDEBUG is defined — assertions are disabled\n");
    return 1;
#endif
    /* Verify ASSERT_MSG actually fires on false condition */
    /* (We can't test this without aborting, so just verify compilation) */
    ASSERT_MSG(1 == 1, "assert_check: sanity check");
    fprintf(stderr, "assert_check: OK — NDEBUG not defined, ASSERT_MSG active\n");
    return 0;
}
