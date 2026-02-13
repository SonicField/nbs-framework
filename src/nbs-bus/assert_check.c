/*
 * assert_check.c — Verify NDEBUG is NOT defined at compile time.
 *
 * ASSERT_MSG is an executable specification, not a debugging aid.
 * If NDEBUG is defined, asserts are silently disabled, which breaks
 * the safety model. This binary catches that at test time.
 */

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
#ifdef NDEBUG
    fprintf(stderr, "FATAL: NDEBUG is defined — asserts are disabled.\n");
    fprintf(stderr, "ASSERT_MSG is an executable specification, not a debugging aid.\n");
    fprintf(stderr, "Remove -DNDEBUG from CFLAGS.\n");
    return 1;
#else
    printf("OK: NDEBUG is not defined — asserts are active.\n");
    return 0;
#endif
}
