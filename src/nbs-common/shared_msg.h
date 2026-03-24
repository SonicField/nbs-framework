/*
 * shared_msg.h — Anonymous shared memory IPC for parent-child processes.
 *
 * Provides a 1024-byte message channel between a parent and a forked
 * child using MAP_SHARED|MAP_ANONYMOUS mmap. No filesystem, no pipes,
 * no signals. Synchronisation via C11 atomics.
 *
 * Usage:
 *   // Before fork:
 *   shared_msg_t *msg = shared_msg_create();
 *
 *   pid_t pid = fork();
 *   if (pid == 0) {
 *       // Child: do work, then send result
 *       shared_msg_send(msg, "a2f31411", 8);
 *       // Child continues or exits — msg survives exec
 *       // (but only if child doesn't exec; after exec, mapping is gone)
 *   } else {
 *       // Parent: wait for child to send
 *       char buf[1024];
 *       int n = shared_msg_recv(msg, buf, sizeof(buf), 15000);
 *       // n > 0: got message, n == 0: timeout, n < 0: error
 *       shared_msg_destroy(msg);
 *   }
 *
 * NOTE: The shared mapping does NOT survive exec. If the child calls
 * exec, it must send the message BEFORE exec. For fork-only children
 * (no exec), the mapping persists until munmap or process exit.
 *
 * For exec children: fork a grandchild that does the work before exec,
 * sends the result, then the intermediate child execs.
 */

#ifndef SHARED_MSG_H
#define SHARED_MSG_H

#include <stddef.h>

/*
 * Opaque handle. Backed by mmap'd anonymous shared memory.
 */
typedef struct shared_msg shared_msg_t;

/*
 * Create a shared message channel.
 * Returns NULL on failure (mmap failed).
 */
shared_msg_t *shared_msg_create(void);

/*
 * Send a message (writer side).
 * data: pointer to message bytes
 * len: message length (max 1024 bytes, truncated if larger)
 *
 * Can be called exactly once. Second call overwrites.
 */
void shared_msg_send(shared_msg_t *msg, const void *data, size_t len);

/*
 * Receive a message (reader side). Blocks until message arrives
 * or timeout expires.
 *
 * buf: destination buffer
 * bufsz: buffer size
 * timeout_ms: max wait in milliseconds (0 = non-blocking check)
 *
 * Returns: bytes copied (> 0), 0 if timeout, -1 on error.
 */
int shared_msg_recv(shared_msg_t *msg, void *buf, size_t bufsz, int timeout_ms);

/*
 * Destroy the channel. Unmaps shared memory.
 * Safe to call from either parent or child (but typically parent).
 */
void shared_msg_destroy(shared_msg_t *msg);

#endif /* SHARED_MSG_H */
