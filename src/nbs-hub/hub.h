/*
 * hub.h — NBS Hub: deterministic process enforcement for AI supervisors.
 *
 * The hub counts, routes, and refuses. It is not intelligent.
 * It cannot drift, skip steps, or get absorbed in tactical work.
 *
 * State layout:
 *   <project>/.nbs/hub/
 *     manifest      Key=value: project_dir, goal, document registry
 *     state         Key=value: phase_num, phase_name, audit_counter,
 *                              audit_required, last_spawn_time
 *     hub.log       Append-only activity log
 *     audits/       Archived audit submissions
 *     gates/        Archived gate submissions
 *   <project>/.nbs/chat/
 *     hub.chat      Hub enforcement log (chat channel)
 *
 * Exit codes:
 *   0 = success
 *   1 = validation error (file missing, incomplete audit, gate refused)
 *   2 = hub not found / document not registered
 *   3 = spawn refused — audit required
 *   4 = usage error (wrong arguments)
 */

#ifndef NBS_HUB_H
#define NBS_HUB_H

/* Exit codes */
#define EXIT_SUCCESS_CODE   0
#define EXIT_ERROR          1
#define EXIT_NOT_FOUND      2
#define EXIT_AUDIT_REQUIRED 3
#define EXIT_BAD_ARGS       4

/* Constants */
#define HUB_SUBDIR          ".nbs/hub"
#define HUB_MANIFEST        ".nbs/hub/manifest"
#define HUB_STATE           ".nbs/hub/state"
#define HUB_LOG             ".nbs/hub/hub.log"
#define HUB_AUDITS          ".nbs/hub/audits"
#define HUB_GATES           ".nbs/hub/gates"
#define HUB_CHAT            ".nbs/chat/hub.chat"

#define AUDIT_THRESHOLD     3
#define STALL_MINUTES       30

#define PATH_BUF_SIZE       4096
#define PATH_JOIN_SIZE      8192  /* For snprintf("%s/%s", path, subdir) */
#define LINE_BUF_SIZE       4096
#define VALUE_BUF_SIZE      4096
#define TIMESTAMP_SIZE      32
#define MAX_DOCS            64
#define MAX_LOG_DEFAULT     20

/* --- State I/O --- */

/*
 * state_read — Read a key=value pair from a state file.
 *
 * Preconditions:
 *   - path, key, out_value are non-NULL
 *   - out_size > 0
 *
 * Postconditions:
 *   - Returns 1 if key found, value written to out_value (NUL-terminated)
 *   - Returns 0 if key not found or file does not exist
 */
int state_read(const char *path, const char *key,
               char *out_value, int out_size);

/*
 * state_write — Write a key=value pair to a state file (atomic).
 *
 * If key exists, replaces the value. If not, appends.
 * Uses write-to-temp + rename for atomicity.
 *
 * Preconditions:
 *   - path, key, value are non-NULL
 *
 * Postconditions:
 *   - Returns 0 on success
 *   - Returns -1 on I/O error
 */
int state_write(const char *path, const char *key, const char *value);

/*
 * hub_log — Append a timestamped entry to hub.log.
 *
 * Preconditions:
 *   - project_dir, message are non-NULL
 *
 * Postconditions:
 *   - Line appended: "[ISO8601] message\n"
 */
void hub_log(const char *project_dir, const char *message);

/*
 * hub_chat — Send a message to hub.chat via nbs-chat.
 *
 * Preconditions:
 *   - project_dir, message are non-NULL
 */
void hub_chat(const char *project_dir, const char *message);

/* --- Utility --- */

/*
 * iso_timestamp — Write current UTC time as ISO 8601 to buf.
 */
void iso_timestamp(char *buf, int size);

/*
 * file_exists — Return 1 if path exists and is a regular file.
 */
int file_exists(const char *path);

/*
 * file_nonempty — Return 1 if path exists and has size > 0.
 */
int file_nonempty(const char *path);

/*
 * file_contains — Return 1 if file at path contains substr.
 */
int file_contains(const char *path, const char *substr);

/*
 * ensure_dir — Create directory if it does not exist.
 */
int ensure_dir(const char *path);

/*
 * find_project_dir — Search upward from cwd for .nbs/hub/.
 *
 * Writes absolute path to out_dir (size PATH_BUF_SIZE).
 * Returns 0 on success, -1 if not found.
 */
int find_project_dir(const char *cwd, char *out_dir);

/* --- Commands --- */

int cmd_init(const char *project_dir, const char *goal);
int cmd_status(const char *project_dir);
int cmd_spawn(const char *slug, const char *task_desc,
              const char *project_dir);
int cmd_check(const char *worker_name, const char *project_dir);
int cmd_result(const char *worker_name, const char *project_dir);
int cmd_dismiss(const char *worker_name, const char *project_dir);
int cmd_list(const char *project_dir);
int cmd_audit(const char *audit_file, const char *project_dir);
int cmd_gate(const char *phase_name, const char *test_file,
             const char *audit_file, const char *project_dir);
int cmd_phase(const char *project_dir);
int cmd_doc_register(const char *name, const char *doc_path,
                     const char *project_dir);
int cmd_doc_list(const char *project_dir);
int cmd_doc_read(const char *name, const char *project_dir);
int cmd_decision(const char *text, const char *project_dir);
int cmd_log(int n, const char *project_dir);
void cmd_help(void);

#endif /* NBS_HUB_H */
