/*
 * helper_client.h — Request a PTY from nbs-ts-helper.
 *
 * Returns the PTY master fd on success, -1 if helper is not running.
 * Caller owns the fd and is responsible for closing it.
 */

#ifndef NBS_TS_HELPER_CLIENT_H
#define NBS_TS_HELPER_CLIENT_H

#include <sys/types.h>

/*
 * helper_request_pty — Ask nbs-ts-helper to spawn a command and return the PTY fd.
 *
 * Connects to ~/.nbs-ts/helper.sock, sends the command, receives the
 * PTY master fd via SCM_RIGHTS and the child PID.
 *
 * Returns: master fd (>= 0) on success, -1 if helper not running or error.
 * If out_child_pid is non-NULL, sets it to the child PID on success.
 */
int helper_request_pty(const char *command, pid_t *out_child_pid);

#endif /* NBS_TS_HELPER_CLIENT_H */
