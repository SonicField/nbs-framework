/*
 * registry.h — Control inbox/registry management.
 *
 * The AI declares resources via the control inbox. The sidecar
 * reads new lines (forward-only) and maintains the registry file.
 */

#ifndef NBS_REGISTRY_H
#define NBS_REGISTRY_H

#include <stddef.h>

/*
 * registry_seed — Populate registry from existing .nbs/ resources.
 *
 * Scans .nbs/chat/ for .chat files and .nbs/events/ directory.
 * Appends new entries to registry file (idempotent).
 * Returns: 0 on success, -1 on error
 */
int registry_seed(const char *nbs_root, const char *registry_path);

/*
 * registry_process_inbox — Read new lines from control inbox, dispatch commands.
 *
 * Forward-only: tracks line offset via *inbox_line, never re-processes.
 * Supported commands: register-chat, unregister-chat, register-bus, unregister-bus.
 * Returns: number of commands processed, or -1 on error
 */
int registry_process_inbox(const char *inbox_path, const char *registry_path,
                            int *inbox_line);

/*
 * registry_find_first — Find the first entry of a given type.
 *
 * type: "chat" or "bus"
 * out: buffer for the path
 * Returns: 0 if found, -1 if not found or I/O error.
 *   On "not found", errno is set to 0. On I/O error, errno is set
 *   by the failing syscall. Callers can check errno to distinguish.
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
 * Returns: number of entries iterated (including the one that
 *   triggered early exit, if any), or -1 on I/O error
 */
int registry_for_each(const char *registry_path, const char *type,
                       int (*callback)(const char *path, void *user_data),
                       void *user_data);

#endif /* NBS_REGISTRY_H */
