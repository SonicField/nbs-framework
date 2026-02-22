/*
 * worker.h — Worker lifecycle management declarations.
 *
 * Defines the interface for all 8 nbs-worker commands:
 *   spawn, status, search, results, dismiss, continue, session, list
 *
 * Exit codes:
 *   0 = success
 *   1 = general error
 *   2 = not found
 *   4 = bad arguments
 */

#ifndef NBS_WORKER_H
#define NBS_WORKER_H

#include <stddef.h>

/* Exit codes */
#define EXIT_SUCCESS_CODE   0
#define EXIT_ERROR          1
#define EXIT_NOT_FOUND      2
#define EXIT_BAD_ARGS       4

/* Constants */
#define TMUX_PREFIX         "pty_"
#define WORKERS_SUBDIR      ".nbs/workers"
#define SESSIONS_SUBDIR     ".nbs/sessions"
#define EVENTS_SUBDIR       ".nbs/events"
#define PIDS_SUBDIR         ".nbs/pids"

#define NAME_MAX_LEN        128
#define PATH_BUF_SIZE       4096
#define LINE_BUF_SIZE       4096
#define CAPTURE_BUF_SIZE    65536
#define TIMESTAMP_SIZE      32

/* --- Validation --- */

/*
 * validate_slug — Check slug matches ^[a-z0-9]+$
 * Returns 1 if valid, 0 if not.
 */
int validate_slug(const char *slug);

/*
 * validate_worker_name — Check name matches ^[a-z0-9]+-[a-f0-9]{4}$
 * Path traversal defence.
 * Returns 1 if valid, 0 if not.
 */
int validate_worker_name(const char *name);

/*
 * validate_uuid — Check string matches UUID format.
 * Returns 1 if valid, 0 if not.
 */
int validate_uuid(const char *s);

/* --- Commands --- */

int cmd_spawn(const char *slug, const char *project_dir,
              const char *task_description, const char *cwd);

int cmd_status(const char *name, const char *cwd);

int cmd_search(const char *name, const char *pattern,
               int context_lines, const char *cwd);

int cmd_results(const char *name, const char *cwd);

int cmd_dismiss(const char *name, const char *cwd);

int cmd_continue(const char *handle, const char *model_override,
                 const char *cwd);

int cmd_session(const char *handle, const char *cwd);

int cmd_list(const char *cwd);

void cmd_help(void);

#endif /* NBS_WORKER_H */
