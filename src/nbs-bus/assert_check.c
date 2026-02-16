/*
 * assert_check.c — Verify NDEBUG is NOT defined at compile time.
 *
 * ASSERT_MSG is an executable specification, not a debugging aid.
 * If NDEBUG is defined, asserts are silently disabled, which breaks
 * the safety model. This binary catches that at test time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
#ifdef NDEBUG
    if (fprintf(stderr, "FATAL: NDEBUG is defined — asserts are disabled.\n") < 0) {
        _exit(1);
    }
    if (fprintf(stderr, "ASSERT_MSG is an executable specification, not a debugging aid.\n") < 0) {
        _exit(1);
    }
    if (fprintf(stderr, "Remove -DNDEBUG from CFLAGS.\n") < 0) {
        _exit(1);
    }
    return 1;
#else
    if (printf("OK: NDEBUG is not defined — asserts are active.\n") < 0) {
        _exit(1);
    }
    return 0;
#endif
}
