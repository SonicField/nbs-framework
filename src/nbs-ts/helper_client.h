/*
 * helper_client.h — Request a PTY from nbs-ts-helper.
 *
 * Returns the PTY master fd on success, -1 if helper is not running.
 * Caller owns the fd and is responsible for closing it.
 */

#ifndef NBS_TS_HELPER_CLIENT_H
#define NBS_TS_HELPER_CLIENT_H

/*
 * helper_request_pty — Ask nbs-ts-helper to spawn a command and return the PTY fd.
 *
 * Connects to ~/.nbs-ts/helper.sock, sends the command, receives the
 * PTY master fd via SCM_RIGHTS.
 *
 * Returns: master fd (>= 0) on success, -1 if helper not running or error.
 */
int helper_request_pty(const char *command);

#endif /* NBS_TS_HELPER_CLIENT_H */
