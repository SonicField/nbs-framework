/*
 * session.h — pty-session command declarations and constants.
 *
 * Each cmd_* function implements one pty-session subcommand.
 * All tmux interaction is via fork+exec (no library linking).
 *
 * Exit codes:
 *   0 = success
 *   1 = general error
 *   2 = session not found
 *   3 = timeout
 *   4 = invalid arguments
 */

#ifndef NBS_PTY_SESSION_H
#define NBS_PTY_SESSION_H

#include <stddef.h>  /* size_t */

#define PTY_PREFIX       "pty_"
#define PTY_PREFIX_LEN   4

#define EXIT_SUCCESS_CODE  0
#define EXIT_ERROR         1
#define EXIT_NOT_FOUND     2
#define EXIT_TIMEOUT       3
#define EXIT_BAD_ARGS      4

#define DEFAULT_SCROLLBACK  100
#define DEFAULT_WAIT_TIMEOUT 60
#define DEFAULT_READ_TIMEOUT 300
#define POLL_INTERVAL_USEC  500000  /* 500ms */

#define MAX_SESSION_NAME   256
#define MAX_PATH_LEN       4096
#define CAPTURE_BUF_SIZE   (256 * 1024)  /* 256 KiB for pane capture */

/*
 * cmd_create — Create a new tmux session running the given command.
 *
 * Preconditions:
 *   - name is non-NULL, non-empty
 *   - command is non-NULL, non-empty
 *   - Session pty_<name> does not already exist
 *
 * Postconditions:
 *   - On success (returns 0): tmux session pty_<name> is running,
 *     pipe-pane logging is active to ~/.pty-session/logs/<name>.log
 *   - On error (returns 1): session was not created
 */
int cmd_create(const char *name, const char *command);

/*
 * cmd_send — Send keystrokes to an existing session.
 *
 * Preconditions:
 *   - name is non-NULL, non-empty
 *   - text is non-NULL (may be empty)
 *   - Session pty_<name> exists
 *
 * Postconditions:
 *   - On success (returns 0): text was sent, Enter appended unless no_enter
 *   - On not found (returns 2): session does not exist
 */
int cmd_send(const char *name, const char *text, int no_enter);

/*
 * cmd_read — Read output from a session (live, cache, or log).
 *
 * Resolution order: live pane -> cache -> persistent log.
 *
 * Preconditions:
 *   - name is non-NULL, non-empty
 *
 * Postconditions:
 *   - On success (returns 0): output printed to stdout
 *   - On not found (returns 2): no live session, cache, or log found
 *   - On timeout (returns 3): wait mode timed out
 */
int cmd_read(const char *name, int scrollback, int wait_mode, int timeout);

/*
 * cmd_wait — Poll until pattern appears in session output.
 *
 * Preconditions:
 *   - name is non-NULL, non-empty
 *   - pattern is non-NULL, non-empty
 *   - Session pty_<name> exists
 *
 * Postconditions:
 *   - On success (returns 0): pattern was found in pane output
 *   - On not found (returns 2): session does not exist
 *   - On timeout (returns 3): pattern not found within timeout
 */
int cmd_wait(const char *name, const char *pattern, int timeout);

/*
 * cmd_kill — Cache session output then terminate the session.
 *
 * Preconditions:
 *   - name is non-NULL, non-empty
 *   - Session pty_<name> exists
 *
 * Postconditions:
 *   - On success (returns 0): session terminated, output cached
 *   - On not found (returns 2): session does not exist
 */
int cmd_kill(const char *name);

/*
 * cmd_list — Show active and killed pty-session sessions.
 *
 * Postconditions:
 *   - Returns 0 always
 *   - Prints session list to stdout
 */
int cmd_list(void);

/*
 * cmd_help — Print usage information to stdout.
 */
int cmd_help(void);

/*
 * is_safe_home_path — Validate that a HOME path contains no shell
 * metacharacters that could enable injection via tmux pipe-pane.
 *
 * Accepts only: [a-zA-Z0-9/_.-]
 * Rejects empty strings and paths containing quotes, backticks,
 * dollar signs, semicolons, pipes, ampersands, etc.
 *
 * Returns 1 if safe, 0 if unsafe.
 */
int is_safe_home_path(const char *path);

#ifdef TEST_BUILD
/*
 * Test-visible wrappers for static functions.
 * Only available when compiled with -DTEST_BUILD.
 */

/* From main.c */
int test_parse_int_option(const char *arg, int default_val);
void test_sanitise_for_display(const char *input, char *buf, size_t bufsize);
int test_dispatch_read(int argc, char *argv[]);
int test_dispatch_wait(int argc, char *argv[]);

/* From session.c */
int test_is_safe_name(const char *name);
int test_is_safe_home_path(const char *path);

#endif /* TEST_BUILD */

#endif /* NBS_PTY_SESSION_H */
