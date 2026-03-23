/*
 * nbs_ts.h — NBS Terminal Service public API.
 *
 * Manages command sessions in pseudo-terminals with reliable output
 * capture and explicit completion signalling. Replaces previous session managers for local
 * AI agent process management.
 *
 * Each session is:
 *   - A PTY pair (master fd for I/O, slave fd connected to the shell)
 *   - An append-only output log (~/.nbs-ts/sessions/<id>/output.log)
 *   - A completion log (~/.nbs-ts/sessions/<id>/completion.log)
 *   - A PID file and metadata
 *
 * Exit codes:
 *   0 = success
 *   1 = general error
 *   2 = session not found
 *   3 = timeout
 *   4 = invalid arguments
 */

#ifndef NBS_TS_H
#define NBS_TS_H

#include <stddef.h>   /* size_t, off_t */
#include <sys/types.h> /* pid_t */

#define NBS_TS_EXIT_SUCCESS    0
#define NBS_TS_EXIT_ERROR      1
#define NBS_TS_EXIT_NOT_FOUND  2
#define NBS_TS_EXIT_TIMEOUT    3
#define NBS_TS_EXIT_BAD_ARGS   4

#define NBS_TS_HANDLE_LEN      16   /* 8 hex chars + NUL, rounded */
#define NBS_TS_MAX_PATH       4096

/* Session status values */
typedef enum {
    NBS_TS_ALIVE,
    NBS_TS_DEAD,
    NBS_TS_UNKNOWN
} nbs_ts_status_t;

/* Completion record from PROMPT_COMMAND */
typedef struct {
    unsigned long seq;
    int exit_code;
} nbs_ts_completion_t;

/* Opaque session handle (in-process use) */
typedef struct nbs_ts_session nbs_ts_session_t;

/* Session creation options */
typedef struct {
    const char *cwd;    /* Working directory (NULL = inherit) */
} nbs_ts_opts_t;

/*
 * nbs_ts_create — Create a new session running the given command.
 *
 * Forks a child process connected to a PTY. Starts the output capture
 * thread and injects PROMPT_COMMAND for completion signalling.
 *
 * Returns session handle on success, NULL on error.
 * The caller must call nbs_ts_destroy() when done.
 */
nbs_ts_session_t *nbs_ts_create(const char *command, const nbs_ts_opts_t *opts);

/*
 * nbs_ts_destroy — Terminate the session and free resources.
 *
 * Sends SIGTERM to the child, closes the PTY, removes the session
 * directory. Safe to call on NULL.
 */
void nbs_ts_destroy(nbs_ts_session_t *s);

/*
 * nbs_ts_send — Write data to the session's PTY.
 *
 * Direct write(2) to the master fd. No keystroke simulation.
 * Returns 0 on success, -1 on error.
 */
int nbs_ts_send(nbs_ts_session_t *s, const char *data, size_t len);

/*
 * nbs_ts_read_new — Read output since last read.
 *
 * Reads from the output log starting at the session's read cursor.
 * Advances the cursor. Returns bytes read, 0 if no new data.
 */
size_t nbs_ts_read_new(nbs_ts_session_t *s, char *buf, size_t max_len);

/*
 * nbs_ts_read — Read output from a specific byte offset.
 *
 * Does not affect the read cursor.
 * Returns bytes read.
 */
size_t nbs_ts_read(nbs_ts_session_t *s, char *buf, size_t max_len, off_t offset);

/*
 * nbs_ts_read_tail — Read the last n_lines lines from the output log.
 *
 * Provides viewport semantics for the sidecar transport.
 * Returns bytes written to buf. NOT NUL-terminated.
 */
size_t nbs_ts_read_tail(nbs_ts_session_t *s, char *buf, size_t max_len,
                        int n_lines);

/*
 * nbs_ts_wait_complete — Wait for the next command completion.
 *
 * Watches the completion log with inotify. Blocks until a new
 * completion record appears or timeout_ms expires.
 *
 * Returns 0 on success (out is filled), -1 on timeout or error.
 */
int nbs_ts_wait_complete(nbs_ts_session_t *s, int timeout_ms,
                         nbs_ts_completion_t *out);

/*
 * nbs_ts_wait_pattern — Wait for a pattern to appear in output.
 *
 * Watches the output log. Returns 0 if pattern found, -1 on timeout.
 */
int nbs_ts_wait_pattern(nbs_ts_session_t *s, const char *pattern,
                        int timeout_ms);

/*
 * nbs_ts_status — Check if the child process is alive.
 */
nbs_ts_status_t nbs_ts_status(nbs_ts_session_t *s);

/*
 * nbs_ts_exit_code — Get the exit code of the child process.
 *
 * Returns the exit code if the child has exited, -1 if still alive.
 */
int nbs_ts_exit_code(nbs_ts_session_t *s);

/*
 * nbs_ts_handle — Get the session's opaque handle string.
 *
 * Returns a pointer to a NUL-terminated string (valid for session lifetime).
 */
const char *nbs_ts_handle(const nbs_ts_session_t *s);

/*
 * nbs_ts_pid — Get the child process PID.
 */
pid_t nbs_ts_pid(const nbs_ts_session_t *s);

/* ── CLI-level functions (used by main.c) ─────────────────────────── */

/*
 * nbs_ts_sessions_dir — Get the sessions directory path.
 *
 * Returns ~/.nbs-ts/sessions/ (creates if needed).
 * buf must be at least NBS_TS_MAX_PATH bytes.
 * Returns 0 on success, -1 on error.
 */
int nbs_ts_sessions_dir(char *buf, size_t bufsize);

/*
 * nbs_ts_session_dir — Get a specific session's directory path.
 *
 * buf must be at least NBS_TS_MAX_PATH bytes.
 * Returns 0 on success, -1 on error.
 */
int nbs_ts_session_dir(const char *handle, char *buf, size_t bufsize);

#endif /* NBS_TS_H */
