/*
 * worker.h — Worker lifecycle management declarations.
 *
 * Defines the interface for all 8 nbs-workers commands:
 *   spawn, status, search, results, dismiss, continue, session, list
 *
 * Exit codes:
 *   0 = success
 *   1 = general error
 *   2 = not found
 *   4 = bad arguments
 */

#ifndef NBS_WORKERS_H
#define NBS_WORKERS_H

#include <stddef.h>

/* Exit codes */
#define EXIT_SUCCESS_CODE   0
#define EXIT_ERROR          1
#define EXIT_NOT_FOUND      2
#define EXIT_BAD_ARGS       4

/* Constants */
#define SESSION_NAME_PREFIX "nbs-"
#define SESSION_NAME_SUFFIX "-worker"
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
 *
 * Preconditions:
 *   - slug != NULL (aborts via ASSERT_MSG if NULL)
 *
 * Postconditions:
 *   - Returns 1 if slug[0] != '\0' and matches ^[a-z0-9]+$
 *   - Returns 0 otherwise
 */
int validate_slug(const char *slug);

/*
 * validate_worker_name — Check name matches ^[a-z0-9]+-[a-f0-9]{4}$
 * Path traversal defence.
 *
 * Preconditions:
 *   - name != NULL (aborts via ASSERT_MSG if NULL)
 *
 * Postconditions:
 *   - Returns 1 if name[0] != '\0' and matches ^[a-z0-9]+-[a-f0-9]{4}$
 *   - Returns 0 otherwise
 */
int validate_worker_name(const char *name);

/*
 * validate_uuid — Check string matches UUID format.
 *
 * Preconditions:
 *   - s != NULL (aborts via ASSERT_MSG if NULL)
 *
 * Postconditions:
 *   - Returns 1 if s matches xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars, lowercase hex)
 *   - Returns 0 otherwise
 */
int validate_uuid(const char *s);

/*
 * validate_safe_handle -- Check handle is safe for shell interpolation.
 *
 * Preconditions:
 *   - handle != NULL (aborts via ASSERT_MSG if NULL)
 *
 * Postconditions:
 *   - Returns 1 if handle matches [a-z0-9][-a-z0-9]* (no leading hyphen)
 *   - Returns 0 otherwise
 *
 * Security boundary: handles are interpolated into shell commands
 * passed to nbs-ts. This function rejects all shell metacharacters.
 */
int validate_safe_handle(const char *handle);

/*
 * validate_safe_model -- Check model name is safe for shell interpolation.
 *
 * Preconditions:
 *   - model != NULL (aborts via ASSERT_MSG if NULL)
 *
 * Postconditions:
 *   - Returns 1 if model matches [a-z0-9][-a-z0-9._:]* (no leading hyphen)
 *   - Returns 0 otherwise
 *
 * Security boundary: model names are interpolated into shell commands.
 */
int validate_safe_model(const char *model);

/* --- Commands --- */

/*
 * cmd_spawn — Create a new worker with an nbs-ts session.
 *
 * Preconditions:
 *   - slug != NULL, slug[0] != '\0', matches ^[a-z0-9]+$
 *   - project_dir != NULL, project_dir[0] != '\0'
 *   - task_description != NULL, task_description[0] != '\0'
 *   - cwd != NULL (unused; spawn uses project_dir as anchor)
 *
 * Postconditions:
 *   - On success (0): worker task file created at <project_dir>/.nbs/workers/<slug>-<4hex>.md,
 *                     nbs-ts session running with name nbs-<slug>-worker-<4hex>
 *   - On error (1): project directory invalid or I/O failure, no worker created
 *   - On bad args (4): slug invalid or required parameters empty/NULL
 */
int cmd_spawn(const char *slug, const char *project_dir,
              const char *task_description, const char *cwd);

/*
 * cmd_status — Display worker status (running/completed/dismissed/unknown).
 *
 * Preconditions:
 *   - name != NULL, name[0] != '\0'
 *   - cwd != NULL
 *
 * Postconditions:
 *   - On success (0): status printed to stdout
 *   - On not found (2): worker task file does not exist
 *   - On bad args (4): name is NULL or empty
 */
int cmd_status(const char *name, const char *cwd);

/*
 * cmd_search — Search worker log for pattern with optional context.
 *
 * Preconditions:
 *   - name != NULL, name[0] != '\0', matches ^[a-z0-9]+-[a-f0-9]{4}$
 *   - pattern != NULL, pattern[0] != '\0', valid regex
 *   - context_lines >= 0
 *   - cwd != NULL
 *
 * Postconditions:
 *   - On success (0): matching lines with context printed to stdout
 *   - On not found (2): log file does not exist
 *   - On bad args (4): name/pattern invalid, or regex syntax error
 *   - On error (1): grep execution failed
 */
int cmd_search(const char *name, const char *pattern,
               int context_lines, const char *cwd);

/*
 * cmd_results — Display worker's logged results from task file.
 *
 * Preconditions:
 *   - name != NULL, name[0] != '\0'
 *   - cwd != NULL
 *
 * Postconditions:
 *   - On success (0): content after "## Log" header printed to stdout
 *   - On not found (2): worker task file does not exist
 *   - On bad args (4): name is NULL or empty
 *   - On error (1): failed to open task file
 */
int cmd_results(const char *name, const char *cwd);

/*
 * cmd_dismiss — Kill worker's nbs-ts session and mark as dismissed.
 *
 * Preconditions:
 *   - name != NULL, name[0] != '\0'
 *   - cwd != NULL
 *
 * Postconditions:
 *   - On success (0): nbs-ts session killed (if running), task file State updated to "dismissed",
 *                     Completed timestamp set if empty
 *   - On not found (2): worker task file does not exist
 *   - On bad args (4): name is NULL or empty
 */
int cmd_dismiss(const char *name, const char *cwd);

/*
 * cmd_continue — Resume an agent session using stored session ID.
 *
 * Preconditions:
 *   - handle != NULL, handle[0] != '\0'
 *   - model_override may be NULL (uses stored model if NULL)
 *   - cwd != NULL
 *
 * Postconditions:
 *   - On success (0): nbs-claude launched with existing session ID, nbs-ts session created
 *   - On not found (2): session metadata file does not exist for handle
 *   - On bad args (4): handle is NULL or empty
 *   - On error (1): failed to read session metadata or execute nbs-claude
 */
int cmd_continue(const char *handle, const char *model_override,
                 const char *cwd);

/*
 * cmd_session — Display session metadata for a handle.
 *
 * Preconditions:
 *   - handle != NULL, handle[0] != '\0'
 *   - cwd != NULL
 *
 * Postconditions:
 *   - On success (0): session metadata printed to stdout (session_id, model, project_root, etc.)
 *   - On not found (2): session metadata file does not exist for handle
 *   - On bad args (4): handle is NULL or empty
 *   - On error (1): failed to read or parse session metadata file
 */
int cmd_session(const char *handle, const char *cwd);

/*
 * cmd_list — List all workers in the current project.
 *
 * Preconditions:
 *   - cwd != NULL
 *
 * Postconditions:
 *   - On success (0): workers listed to stdout with status, or "(no workers)" if none exist
 *   - Never returns error codes (missing workers directory is not an error)
 */
int cmd_list(const char *cwd);

void cmd_help(void);

#endif /* NBS_WORKERS_H */
