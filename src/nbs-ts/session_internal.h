/*
 * session_internal.h — Internal session structure and accessor declarations.
 *
 * Only included by session.c, io.c, wait.c, and main.c.
 * Not part of the public API.
 */

#ifndef NBS_TS_SESSION_INTERNAL_H
#define NBS_TS_SESSION_INTERNAL_H

#include "nbs_ts.h"
#include <pthread.h>

/*
 * MAX_FILE_PATH — Buffer size for fully-qualified file paths.
 * Larger than NBS_TS_MAX_PATH to accommodate directory + filename
 * without triggering format-truncation warnings under -Werror.
 */
#define NBS_TS_MAX_FILE_PATH 8192

struct nbs_ts_session {
    char handle[NBS_TS_HANDLE_LEN];
    char session_dir[NBS_TS_MAX_FILE_PATH];
    char output_log_path[NBS_TS_MAX_FILE_PATH];
    char completion_log_path[NBS_TS_MAX_FILE_PATH];

    int master_fd;
    pid_t child_pid;

    pthread_t capture_thread;
    int capture_running;

    off_t read_cursor;
    unsigned long completion_cursor;

    int child_exited;
    int child_exit_code;
};

#endif /* NBS_TS_SESSION_INTERNAL_H */
