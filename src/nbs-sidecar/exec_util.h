/*
 * exec_util.h — Generic fork+exec+capture utilities.
 *
 * Generalises the fork+exec pattern from bus_bridge.c for use by
 * all sidecar modules that need to run external commands.
 */

#ifndef NBS_EXEC_UTIL_H
#define NBS_EXEC_UTIL_H

#include <stddef.h>

/*
 * exec_capture — Fork+exec a command, capture stdout to buffer.
 *
 * Preconditions:
 *   - argv[0] != NULL (program name)
 *   - argv is NULL-terminated
 *   - out_buf != NULL, out_size > 0
 *
 * Postconditions:
 *   - On success (return >= 0): out_buf contains NUL-terminated stdout,
 *     return value is the child exit code
 *   - On failure (return -1): fork/exec failed, out_buf contents undefined
 *
 * stderr is redirected to /dev/null.
 */
int exec_capture(const char *const argv[], char *out_buf, size_t out_size);

/*
 * exec_fire_and_forget — Fork+exec without capturing output.
 *
 * Preconditions:
 *   - argv[0] != NULL (program name)
 *   - argv is NULL-terminated
 *
 * Postconditions:
 *   - Returns child exit code (0 on success), or -1 if fork/exec failed
 *
 * Both stdout and stderr redirected to /dev/null.
 */
int exec_fire_and_forget(const char *const argv[]);

#endif /* NBS_EXEC_UTIL_H */
