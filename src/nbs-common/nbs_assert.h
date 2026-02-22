/*
 * nbs_assert.h — Shared assertion macro for all NBS components.
 *
 * Unlike standard assert(), this macro:
 *   - Always fires (not gated by NDEBUG) — asserts are executable specifications
 *   - Prints file, line, and a formatted message with context values
 *   - Calls abort() for a core dump
 *
 * Usage: ASSERT_MSG(ptr != NULL, "chat_read: path is NULL")
 *        ASSERT_MSG(count >= 0, "message_count went negative: %d", count)
 */

#ifndef NBS_ASSERT_H
#define NBS_ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#define ASSERT_MSG(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERT FAILED %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        abort(); \
    } \
} while(0)

#endif /* NBS_ASSERT_H */
