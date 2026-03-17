/*
 * registry.h — Control inbox/registry management.
 *
 * The AI declares resources via the control inbox. The sidecar
 * reads new lines (forward-only, monotonically advancing line offset)
 * and maintains the registry file.
 *
 * Forward-only invariant: *inbox_line is monotonically non-decreasing
 * across calls to registry_process_inbox. This is enforced by an
 * assertion in the implementation.
 */

#ifndef NBS_REGISTRY_H
#define NBS_REGISTRY_H

#include <stddef.h>

/*
 * registry_seed — Populate registry from existing .nbs/ resources.
 *
 * Scans .nbs/chat/ for .chat files and .nbs/events/ directory.
 * Appends new entries to registry file (idempotent).
 *
 * Preconditions:
 *   - nbs_root != NULL
 *   - registry_path != NULL
 *
 * Returns: 0 on success, -1 on error
 */
int registry_seed(const char *nbs_root, const char *registry_path);

/*
 * registry_process_inbox — Read new lines from control inbox, dispatch commands.
 *
 * Forward-only: tracks line offset via *inbox_line, never re-processes.
 * Supported commands: register-chat, unregister-chat, register-bus, unregister-bus.
 *
 * Preconditions:
 *   - inbox_path != NULL
 *   - registry_path != NULL
 *   - inbox_line != NULL
 *   - *inbox_line >= 0
 *
 * Returns: number of commands processed, or -1 on error
 */
int registry_process_inbox(const char *inbox_path, const char *registry_path,
                            int *inbox_line);

/*
 * registry_find_first — Find the first entry of a given type.
 *
 * type: "chat" or "bus"
 * out: buffer for the path
 *
 * Preconditions:
 *   - registry_path != NULL
 *   - type != NULL
 *   - out != NULL
 *   - out_size > 0
 *
 * Returns:
 *   0  — found (path written to out)
 *  -1  — not found (out unchanged, errno unchanged)
 *  -1  — I/O error (errno set by failing syscall)
 *  -1  — path truncation (path too long for out_size)
 *
 * B23 fix: callers should NOT rely on errno to distinguish "not found"
 * from I/O error. The return value is -1 in both cases. If distinction
 * is required, check whether the registry file exists before calling.
 */
int registry_find_first(const char *registry_path, const char *type,
                         char *out, size_t out_size);

/*
 * registry_for_each — Iterate over all entries of a given type.
 *
 * type: "chat" or "bus"
 * callback: called for each entry with the path and user data.
 *   Return 0 to continue iteration, non-zero for early exit.
 *   Non-zero is an early-exit signal, not an error indication.
 *
 * Preconditions:
 *   - registry_path != NULL
 *   - type != NULL
 *   - callback != NULL (B24: NULL callback will abort via ASSERT_MSG)
 *
 * Returns: number of entries iterated (including the one that
 *   triggered early exit, if any), or -1 on I/O error
 */
int registry_for_each(const char *registry_path, const char *type,
                       int (*callback)(const char *path, void *user_data),
                       void *user_data);

#endif /* NBS_REGISTRY_H */
