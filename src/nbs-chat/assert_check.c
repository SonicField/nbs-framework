/*
 * assert_check.c — Verify that asserts are enabled (NDEBUG is not defined).
 *
 * Compiled with the same CFLAGS as the main binaries.
 * If NDEBUG is defined, standard assert() calls become no-ops,
 * which would silently disable a key safety mechanism.
 *
 * Exit codes:
 *   0 - PASS: asserts are enabled
 *   1 - FAIL: NDEBUG is defined (asserts disabled)
 */

#include <assert.h>
#include <stdio.h>

int main(void) {
#ifdef NDEBUG
    fprintf(stderr, "FAIL: NDEBUG is defined — standard asserts are disabled\n");
    return 1;
#else
    printf("PASS: asserts enabled (NDEBUG not defined)\n");
    return 0;
#endif
}
