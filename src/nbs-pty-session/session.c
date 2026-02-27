/*
 * session.c — pty-session command implementations.
 *
 * All tmux interaction is via fork+exec+pipe (no library linking).
 * The exec_capture/exec_fire_and_forget pattern is copied from
 * nbs-sidecar/exec_util.c.
 *
 * Invariants:
 *   - Child processes always use _exit(), never exit()
 *   - stderr is redirected to /dev/null in children
 *   - waitpid retries on EINTR
 *   - All string formatting via snprintf (never sprintf/strcat)
 *   - Manual resource cleanup on all error paths
 *   - Cache files are created with mode 0600 (not world-readable)
 *   - $HOME is validated for shell metacharacters before use
 *   - Timeouts use CLOCK_MONOTONIC, not accumulated sleep
 */

#include "session.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>

/*
 * MAX_FILE_PATH — Buffer size for fully-qualified file paths.
 *
 * Larger than MAX_PATH_LEN to accommodate directory + filename + extension
 * without triggering format-truncation warnings.
 */
#define MAX_FILE_PATH  8192

/* ── Fork+exec utilities (pattern from nbs-sidecar/exec_util.c) ───── */

/*
 * redirect_stderr_to_devnull — Redirect stderr to /dev/null in child.
 *
 * If /dev/null is unavailable, close stderr outright. This is acceptable
 * because the child immediately calls execvp() after this function
 * returns — no writes to stderr should occur between here and execvp().
 * The fallback (closing stderr) prevents the child from writing to the
 * parent's terminal; the semantic difference from /dev/null redirection
 * is immaterial given the immediate exec. (Violation S10 documentation.)
 */
static void redirect_stderr_to_devnull(void)
{
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
        if (dup2(fd, STDERR_FILENO) < 0) {
            close(fd);
            _exit(126);
        }
        close(fd);
    } else {
        close(STDERR_FILENO);
    }
}

/*
 * exec_capture — Fork+exec a command, capture stdout to buffer.
 *
 * Preconditions:
 *   - argv[0] != NULL (program name)
 *   - argv is NULL-terminated
 *   - out_buf != NULL, out_size > 0
 *
 * Postconditions:
 *   - On success (return >= 0): out_buf contains NUL-terminated stdout,
 *     return value is the child exit code
 *   - On read error during capture (return -1): partial output may be
 *     in out_buf but the return value signals failure
 *   - On fork/exec failure (return -1): out_buf contents undefined
 */
static int exec_capture(const char *const argv[], char *out_buf, size_t out_size)
{
    ASSERT_MSG(argv != NULL && argv[0] != NULL,
               "exec_capture: argv or argv[0] is NULL");
    ASSERT_MSG(out_buf != NULL, "exec_capture: out_buf is NULL");
    ASSERT_MSG(out_size > 0, "exec_capture: out_size is 0");

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child process */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(126);
        }
        close(pipefd[1]);
        redirect_stderr_to_devnull();

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent process */
    close(pipefd[1]);

    size_t total = 0;
    int read_error = 0;  /* Violation S3 fix: track read errors */
    while (total < out_size - 1) {
        ssize_t n = read(pipefd[0], out_buf + total, out_size - 1 - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            /* Violation S3 fix: flag read error instead of silently breaking */
            read_error = 1;
            fprintf(stderr, "Warning: read() failed during capture: %s\n",
                    strerror(errno));
            break;
        }
        if (n == 0)
            break;
        total += (size_t)n;
    }
    out_buf[total] = '\0';
    close(pipefd[0]);

    int status;
    pid_t wpid;
    do {
        wpid = waitpid(pid, &status, 0);
    } while (wpid < 0 && errno == EINTR);

    if (wpid < 0) {
        return -1;
    }

    /* Violation S3 fix: return -1 if read failed, even if child exited OK */
    if (read_error) {
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return -1;
}

/*
 * exec_fire_and_forget — Fork+exec without capturing output.
 *
 * Preconditions:
 *   - argv[0] != NULL
 *   - argv is NULL-terminated
 *
 * Postconditions:
 *   - Returns child exit code (0 on success), or -1 if fork/exec failed
 */
static int exec_fire_and_forget(const char *const argv[])
{
    ASSERT_MSG(argv != NULL && argv[0] != NULL,
               "exec_fire_and_forget: argv or argv[0] is NULL");

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
                close(fd);
                _exit(126);
            }
            close(fd);
        } else {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        }

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    int status;
    pid_t wpid;
    do {
        wpid = waitpid(pid, &status, 0);
    } while (wpid < 0 && errno == EINTR);

    if (wpid < 0) {
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return -1;
}

/* ── Path safety ──────────────────────────────────────────────────── */

/*
 * is_safe_home_path — Validate that a HOME path contains no shell
 * metacharacters that could enable injection via tmux pipe-pane.
 *
 * Violation S1 fix: $HOME is no longer trusted without validation.
 *
 * Accepts only: [a-zA-Z0-9/_.-]
 * Rejects empty strings and paths containing quotes, backticks,
 * dollar signs, semicolons, pipes, ampersands, spaces, etc.
 *
 * Returns 1 if safe, 0 if unsafe.
 */
int is_safe_home_path(const char *path)
{
    ASSERT_MSG(path != NULL, "is_safe_home_path: path is NULL");

    if (path[0] == '\0') {
        return 0;
    }

    for (const char *p = path; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '/' || *p == '_' ||
              *p == '-' || *p == '.')) {
            return 0;
        }
    }
    return 1;
}

/* ── Path resolution ──────────────────────────────────────────────── */

/*
 * resolve_home_path — Build a path under $HOME.
 *
 * Preconditions:
 *   - buf != NULL, bufsize > 0
 *   - suffix is non-NULL
 *   - $HOME is set in the environment
 *   - $HOME contains only safe path characters (no shell metacharacters)
 *
 * Postconditions:
 *   - On success (returns 0): buf contains "$HOME/<suffix>"
 *   - On failure (returns -1): $HOME not set, unsafe, or path too long
 */
static int resolve_home_path(char *buf, size_t bufsize, const char *suffix)
{
    ASSERT_MSG(buf != NULL, "resolve_home_path: buf is NULL");
    ASSERT_MSG(bufsize > 0, "resolve_home_path: bufsize is 0");
    ASSERT_MSG(suffix != NULL, "resolve_home_path: suffix is NULL");

    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME environment variable not set\n");
        return -1;
    }

    /* Violation S1 fix: validate $HOME for shell metacharacters */
    if (!is_safe_home_path(home)) {
        fprintf(stderr, "Error: HOME path contains unsafe characters: "
                "only [a-zA-Z0-9/_.-] are allowed\n");
        return -1;
    }

    int n = snprintf(buf, bufsize, "%s/%s", home, suffix);
    if (n < 0 || (size_t)n >= bufsize) {
        fprintf(stderr, "Error: Path too long\n");
        return -1;
    }

    return 0;
}

/*
 * build_session_name — Build the prefixed tmux session name.
 *
 * Postconditions:
 *   - On success (returns 0): buf contains "pty_<name>"
 *   - On failure (returns -1): name too long
 */
static int build_session_name(char *buf, size_t bufsize, const char *name)
{
    ASSERT_MSG(buf != NULL, "build_session_name: buf is NULL");
    ASSERT_MSG(name != NULL, "build_session_name: name is NULL");
    ASSERT_MSG(bufsize > 0, "build_session_name: bufsize is 0");

    int n = snprintf(buf, bufsize, "%s%s", PTY_PREFIX, name);
    if (n < 0 || (size_t)n >= bufsize) {
        fprintf(stderr, "Error: Session name too long\n");
        return -1;
    }

    return 0;
}

/*
 * ensure_dir — Create directory (and parents) if it doesn't exist.
 *
 * Uses mkdir -p via fork+exec to handle parent creation.
 */
static int ensure_dir(const char *path)
{
    ASSERT_MSG(path != NULL, "ensure_dir: path is NULL");

    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0;
    }

    const char *argv[] = {"mkdir", "-p", path, NULL};
    return exec_fire_and_forget(argv);
}

/* ── tmux helpers ─────────────────────────────────────────────────── */

/*
 * session_exists — Check if a tmux session exists.
 *
 * Returns 1 if session exists, 0 if not.
 */
static int session_exists(const char *session_name)
{
    ASSERT_MSG(session_name != NULL, "session_exists: session_name is NULL");

    const char *argv[] = {"tmux", "has-session", "-t", session_name, NULL};
    return (exec_fire_and_forget(argv) == 0) ? 1 : 0;
}

/*
 * capture_pane — Capture tmux pane content to buffer.
 *
 * Postconditions:
 *   - On success (returns 0): buf contains pane content (NUL-terminated)
 *   - On failure (returns -1): tmux command failed
 */
static int capture_pane(const char *session_name, int scrollback,
                        char *buf, size_t bufsize)
{
    ASSERT_MSG(session_name != NULL, "capture_pane: session_name is NULL");
    ASSERT_MSG(buf != NULL, "capture_pane: buf is NULL");
    ASSERT_MSG(bufsize > 0, "capture_pane: bufsize is 0");
    ASSERT_MSG(scrollback > 0, "capture_pane: scrollback must be positive, got %d", scrollback);

    char scroll_arg[32];
    int n = snprintf(scroll_arg, sizeof(scroll_arg), "-%d", scrollback);
    ASSERT_MSG(n >= 0 && (size_t)n < sizeof(scroll_arg), "scroll_arg truncated");


    const char *argv[] = {
        "tmux", "capture-pane", "-t", session_name,
        "-p", "-S", scroll_arg, NULL
    };

    int rc = exec_capture(argv, buf, bufsize);
    return (rc == 0) ? 0 : -1;
}

/* ── Monotonic time helper ────────────────────────────────────────── */

/*
 * get_monotonic_ms — Get current CLOCK_MONOTONIC time in milliseconds.
 *
 * Violation S6 fix: timeouts now use wall-clock time rather than
 * accumulated nominal sleep intervals.
 *
 * Returns milliseconds since some fixed epoch, or -1 on failure.
 */
static long get_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return -1;
    }
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/* ── Cache operations ─────────────────────────────────────────────── */

/*
 * open_secure — Open a file with explicit permissions (mode 0600).
 *
 * Violation S9 fix: cache files are no longer created world-readable.
 *
 * Returns FILE* on success, NULL on failure.
 */
static FILE *open_secure(const char *path, int flags)
{
    int fd = open(path, flags, 0600);
    if (fd < 0) {
        return NULL;
    }
    const char *mode = (flags & O_RDONLY) ? "r" : "w";
    FILE *f = fdopen(fd, mode);
    if (!f) {
        close(fd);
        return NULL;
    }
    return f;
}

/*
 * cache_session — Snapshot pane content to cache file.
 *
 * Writes pane content to ~/.pty-session/cache/<name>.output
 * and a timestamp to ~/.pty-session/cache/<name>.timestamp.
 */
static int cache_session(const char *name, const char *session_name)
{
    ASSERT_MSG(name != NULL, "cache_session: name is NULL");
    ASSERT_MSG(session_name != NULL, "cache_session: session_name is NULL");
    int n;

    char cache_dir[MAX_PATH_LEN];
    if (resolve_home_path(cache_dir, sizeof(cache_dir), ".pty-session/cache") < 0) {
        return -1;
    }

    if (ensure_dir(cache_dir) != 0) {
        return -1;
    }

    /* Capture pane content */
    char *buf = malloc(CAPTURE_BUF_SIZE);
    if (!buf) {
        /* Violation S12 fix: include allocation size in error message */
        fprintf(stderr, "Error: malloc failed for %zu bytes (capture buffer)\n",
                (size_t)CAPTURE_BUF_SIZE);
        return -1;
    }

    if (capture_pane(session_name, DEFAULT_SCROLLBACK, buf, CAPTURE_BUF_SIZE) < 0) {
        free(buf);
        return -1;
    }

    /* Write output file — Violation S9 fix: use mode 0600 */
    char output_path[MAX_FILE_PATH];
    n = snprintf(output_path, sizeof(output_path), "%s/%s.output", cache_dir, name);
    ASSERT_MSG(n >= 0 && (size_t)n < sizeof(output_path), "output_path truncated");

    FILE *f = open_secure(output_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        free(buf);
        return -1;
    }
    if (fputs(buf, f) == EOF) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    free(buf);

    /* Write timestamp file — Violation S9 fix: use mode 0600 */
    char ts_path[MAX_FILE_PATH];
    n = snprintf(ts_path, sizeof(ts_path), "%s/%s.timestamp", cache_dir, name);
    ASSERT_MSG(n >= 0 && (size_t)n < sizeof(ts_path), "ts_path truncated");

    f = open_secure(ts_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        return -1;
    }
    if (fprintf(f, "%ld\n", (long)time(NULL)) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);

    return 0;
}

/*
 * read_cache — Read and consume cached session output.
 *
 * Prints cache content to stdout, then deletes the cache files.
 * Returns 0 on success, -1 if no cache exists or read error occurs.
 *
 * Violation S13 fix: checks ferror() after fgets loop; does not delete
 * cache files on read error.
 */
static int read_cache(const char *name)
{
    ASSERT_MSG(name != NULL, "read_cache: name is NULL");
    int n;

    char cache_dir[MAX_PATH_LEN];
    if (resolve_home_path(cache_dir, sizeof(cache_dir), ".pty-session/cache") < 0) {
        return -1;
    }

    char output_path[MAX_FILE_PATH];
    n = snprintf(output_path, sizeof(output_path), "%s/%s.output", cache_dir, name);
    ASSERT_MSG(n >= 0 && (size_t)n < sizeof(output_path), "output_path truncated");

    FILE *f = fopen(output_path, "r");
    if (!f) {
        return -1;
    }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (fputs(line, stdout) == EOF) {
            fclose(f);
            return -1;
        }
    }

    /* Violation S13 fix: check for read error before deleting cache */
    if (ferror(f)) {
        fprintf(stderr, "Warning: read error on cache file %s: %s\n",
                output_path, strerror(errno));
        fclose(f);
        return -1;  /* Do not delete cache if read failed */
    }
    fclose(f);

    /* Delete cache files after successful read */
    char ts_path[MAX_FILE_PATH];
    n = snprintf(ts_path, sizeof(ts_path), "%s/%s.timestamp", cache_dir, name);
    ASSERT_MSG(n >= 0 && (size_t)n < sizeof(ts_path), "ts_path truncated");
    if (unlink(output_path) != 0) {
        fprintf(stderr, "Warning: Failed to remove cache file %s: %s\n",
                output_path, strerror(errno));
    }
    if (unlink(ts_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "Warning: Failed to remove timestamp file %s: %s\n",
                ts_path, strerror(errno));
    }

    return 0;
}

/*
 * read_log — Read persistent log file, stripping ANSI escape codes.
 *
 * Uses sed via fork+exec to strip escapes, matching the bash version.
 * Returns 0 on success, -1 if no log exists or sed fails.
 *
 * Violation S5 fix: only treats sed exit code 0 as success.
 */
static int read_log(const char *name)
{
    ASSERT_MSG(name != NULL, "read_log: name is NULL");
    int n;

    char log_dir[MAX_PATH_LEN];
    if (resolve_home_path(log_dir, sizeof(log_dir), ".pty-session/logs") < 0) {
        return -1;
    }

    char full_path[MAX_FILE_PATH];
    n = snprintf(full_path, sizeof(full_path), "%s/%s.log", log_dir, name);
    ASSERT_MSG(n >= 0 && (size_t)n < sizeof(full_path), "full_path truncated");

    if (access(full_path, F_OK) != 0) {
        return -1;
    }

    fprintf(stderr, "(from persistent log)\n");

    /* Strip ANSI escape codes via sed, matching the bash version */
    const char *argv[] = {
        "sed",
        "s/\x1b\\[[0-9;]*[a-zA-Z]//g; s/\x1b\\][^\x07]*\x07//g; s/\x1b[()][0-9A-B]//g",
        full_path,
        NULL
    };

    char *buf = malloc(CAPTURE_BUF_SIZE);
    if (!buf) {
        fprintf(stderr, "Error: malloc failed for %zu bytes (log buffer)\n",
                (size_t)CAPTURE_BUF_SIZE);
        return -1;
    }

    int rc = exec_capture(argv, buf, CAPTURE_BUF_SIZE);
    /* Violation S5 fix: only accept exit code 0, not any non-negative value */
    if (rc == 0) {
        if (fputs(buf, stdout) == EOF) {
            free(buf);
            return -1;
        }
    }
    free(buf);

    return (rc == 0) ? 0 : -1;
}

/* ── Tool reminder header ─────────────────────────────────────────── */

/*
 * show_tool_header — Print higher-level tool reminder to stderr.
 *
 * Suppressed when NBS_PTY_QUIET=1 is set in the environment.
 */
static void show_tool_header(void)
{
    const char *quiet = getenv("NBS_PTY_QUIET");
    if (quiet && strcmp(quiet, "1") == 0) {
        return;
    }

    fprintf(stderr,
        "**pty-session: consider higher-level tools**\n"
        "- Read files:  `nbs-remote-read <host> <path> [--head=N|--tail=N]`\n"
        "- Edit files:  `nbs-remote-edit pull/push/diff <host> <path>`\n"
        "- Build:       `nbs-remote-build <ses> '<cmd>' --chat=... --handle`\n"
        "- Git diff:    `nbs-remote-diff <ses> --cwd=<dir> --commit=<ref>`\n"
        "- Git status:  `nbs-remote-status <ses> --cwd=<dir>`\n"
        "- Lock:        `pty-session-lock acquire/release <ses> <handle>`\n"
        "- Suppress:    `export NBS_PTY_QUIET=1`\n"
    );
}

/* ── Log search helper ────────────────────────────────────────────── */

/*
 * search_log_for_pattern — Search persistent log for a text pattern.
 *
 * Violation S8 fix: extracted from duplicated code in cmd_wait().
 * Used both when session is already gone on entry and when session
 * exits during polling.
 *
 * Returns EXIT_SUCCESS_CODE if pattern found, EXIT_NOT_FOUND if not.
 */
static int search_log_for_pattern(const char *name, const char *pattern)
{
    ASSERT_MSG(name != NULL, "search_log_for_pattern: name is NULL");
    ASSERT_MSG(pattern != NULL, "search_log_for_pattern: pattern is NULL");

    char log_path[MAX_PATH_LEN];
    if (resolve_home_path(log_path, sizeof(log_path),
                           ".pty-session/logs") != 0) {
        return EXIT_NOT_FOUND;
    }

    char full_log[MAX_PATH_LEN];
    int n = snprintf(full_log, sizeof(full_log), "%s/%s.log", log_path, name);
    if (n <= 0 || (size_t)n >= sizeof(full_log)) {
        return EXIT_NOT_FOUND;
    }

    FILE *f = fopen(full_log, "r");
    if (!f) {
        return EXIT_NOT_FOUND;
    }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, pattern) != NULL) {
            fclose(f);
            printf("Pattern found in log after session exit\n");
            return EXIT_SUCCESS_CODE;
        }
    }
    fclose(f);

    return EXIT_NOT_FOUND;
}

/* ── Command implementations ──────────────────────────────────────── */

/*
 * is_safe_name — Validate session name contains only safe characters.
 *
 * Accepts only [a-zA-Z0-9_-]. Rejects empty names and names containing
 * shell metacharacters, path separators, or any other characters that
 * could enable command injection (see V2.6 in audit report).
 *
 * Returns 1 if safe, 0 if unsafe.
 */
static int is_safe_name(const char *name)
{
    ASSERT_MSG(name != NULL, "is_safe_name: name is NULL");

    if (name[0] == '\0') {
        return 0;
    }

    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            return 0;
        }
    }
    return 1;
}

int cmd_create(const char *name, const char *command)
{
    ASSERT_MSG(name != NULL, "cmd_create: name is NULL");
    ASSERT_MSG(command != NULL, "cmd_create: command is NULL");
    int n;

    if (name[0] == '\0' || command[0] == '\0') {
        fprintf(stderr, "Error: create requires <name> and <command>\n");
        fprintf(stderr, "Usage: pty-session create <name> <command>\n");
        return EXIT_BAD_ARGS;
    }

    if (!is_safe_name(name)) {
        fprintf(stderr,
                "Error: Session name '%s' contains invalid characters "
                "(only [a-zA-Z0-9_-] allowed)\n", name);
        return EXIT_BAD_ARGS;
    }

    char session[MAX_SESSION_NAME];
    if (build_session_name(session, sizeof(session), name) < 0) {
        return EXIT_ERROR;
    }

    if (session_exists(session)) {
        fprintf(stderr, "Error: Session '%s' already exists\n", name);
        return EXIT_ERROR;
    }

    /* tmux new-session -d -s pty_<name> <command> */
    const char *create_argv[] = {
        "tmux", "new-session", "-d", "-s", session, command, NULL
    };
    int rc = exec_fire_and_forget(create_argv);
    if (rc != 0) {
        fprintf(stderr, "Error: Failed to create tmux session\n");
        return EXIT_ERROR;
    }

    /* Set up persistent logging: tmux pipe-pane -t pty_<name> -o "cat >> <logfile>" */
    char log_dir[MAX_PATH_LEN];
    if (resolve_home_path(log_dir, sizeof(log_dir), ".pty-session/logs") < 0) {
        return EXIT_ERROR;
    }
    if (ensure_dir(log_dir) != 0) {
        fprintf(stderr, "Warning: Failed to create log directory\n");
        /* Non-fatal: session is already created */
    }

    char pipe_cmd[MAX_FILE_PATH];
    n = snprintf(pipe_cmd, sizeof(pipe_cmd), "cat >> '%s/%s.log'", log_dir, name);
    ASSERT_MSG(n >= 0 && (size_t)n < sizeof(pipe_cmd), "pipe_cmd truncated");

    const char *pipe_argv[] = {
        "tmux", "pipe-pane", "-t", session, "-o", pipe_cmd, NULL
    };
    int pipe_rc = exec_fire_and_forget(pipe_argv);
    if (pipe_rc != 0) {
        fprintf(stderr, "Warning: Failed to set up pipe-pane logging\n");
    }

    printf("Created session: %s\n", name);
    return EXIT_SUCCESS_CODE;
}

int cmd_send(const char *name, const char *text, int no_enter)
{
    ASSERT_MSG(name != NULL, "cmd_send: name is NULL");
    ASSERT_MSG(text != NULL, "cmd_send: text is NULL");

    if (name[0] == '\0') {
        fprintf(stderr, "Error: send requires <name> and <text>\n");
        return EXIT_BAD_ARGS;
    }

    if (!is_safe_name(name)) {
        fprintf(stderr,
                "Error: Session name '%s' contains invalid characters "
                "(only [a-zA-Z0-9_-] allowed)\n", name);
        return EXIT_BAD_ARGS;
    }

    show_tool_header();

    char session[MAX_SESSION_NAME];
    if (build_session_name(session, sizeof(session), name) < 0) {
        return EXIT_ERROR;
    }

    if (!session_exists(session)) {
        fprintf(stderr, "Error: Session '%s' not found\n", name);
        return EXIT_NOT_FOUND;
    }

    /* tmux send-keys -t pty_<name> -l <text> */
    const char *send_argv[] = {
        "tmux", "send-keys", "-t", session, "-l", text, NULL
    };
    int rc = exec_fire_and_forget(send_argv);
    if (rc != 0) {
        fprintf(stderr, "Error: Failed to send keys\n");
        return EXIT_ERROR;
    }

    /* Send Enter unless --no-enter specified */
    if (!no_enter) {
        /* Small delay for reliable submission, matching bash version */
        usleep(100000); /* 0.1s */

        const char *enter_argv[] = {
            "tmux", "send-keys", "-t", session, "Enter", NULL
        };
        rc = exec_fire_and_forget(enter_argv);
        if (rc != 0) {
            fprintf(stderr, "Error: Failed to send Enter\n");
            return EXIT_ERROR;
        }
    }

    return EXIT_SUCCESS_CODE;
}

int cmd_read(const char *name, int scrollback, int wait_mode, int timeout)
{
    ASSERT_MSG(name != NULL, "cmd_read: name is NULL");
    ASSERT_MSG(scrollback > 0, "cmd_read: scrollback must be positive, got %d", scrollback);
    ASSERT_MSG(timeout > 0, "cmd_read: timeout must be positive, got %d", timeout);
    ASSERT_MSG(timeout <= 100000,
               "cmd_read: timeout out of range: %d", timeout);

    if (name[0] == '\0') {
        fprintf(stderr, "Error: read requires <name>\n");
        return EXIT_BAD_ARGS;
    }

    if (!is_safe_name(name)) {
        fprintf(stderr,
                "Error: Session name '%s' contains invalid characters "
                "(only [a-zA-Z0-9_-] allowed)\n", name);
        return EXIT_BAD_ARGS;
    }

    show_tool_header();

    char session[MAX_SESSION_NAME];
    if (build_session_name(session, sizeof(session), name) < 0) {
        return EXIT_ERROR;
    }

    /* Wait mode: poll until session exits, then read cache */
    if (wait_mode) {
        /* Violation S6 fix: use CLOCK_MONOTONIC for timeout */
        long timeout_ms = (long)timeout * 1000;
        /* Violation S7 fix: postcondition on multiplication */
        ASSERT_MSG(timeout_ms > 0 && timeout_ms / 1000 == (long)timeout,
                   "timeout_ms overflow: timeout=%d, timeout_ms=%ld",
                   timeout, timeout_ms);

        long start_ms = get_monotonic_ms();
        ASSERT_MSG(start_ms >= 0, "clock_gettime(CLOCK_MONOTONIC) failed");

        while (session_exists(session)) {
            long now_ms = get_monotonic_ms();
            if (now_ms < 0 || (now_ms - start_ms) >= timeout_ms) {
                fprintf(stderr, "Error: Timeout after %ds waiting for session to exit\n",
                        timeout);
                return EXIT_TIMEOUT;
            }
            usleep(POLL_INTERVAL_USEC);
        }

        /* Session has exited, try cache then log */
        if (read_cache(name) == 0) {
            return EXIT_SUCCESS_CODE;
        }
        if (read_log(name) == 0) {
            return EXIT_SUCCESS_CODE;
        }

        fprintf(stderr, "Error: Session exited but no output found (no cache or log)\n");
        return EXIT_NOT_FOUND;
    }

    /* Non-wait mode: try live session first */
    if (session_exists(session)) {
        char *buf = malloc(CAPTURE_BUF_SIZE);
        if (!buf) {
            fprintf(stderr, "Error: malloc failed for %zu bytes (capture buffer)\n",
                    (size_t)CAPTURE_BUF_SIZE);
            return EXIT_ERROR;
        }

        if (capture_pane(session, scrollback, buf, CAPTURE_BUF_SIZE) == 0) {
            /* Violation S4 fix: check fputs return value */
            if (fputs(buf, stdout) == EOF) {
                free(buf);
                fprintf(stderr, "Error: failed to write output to stdout\n");
                return EXIT_ERROR;
            }
            free(buf);
            return EXIT_SUCCESS_CODE;
        }
        free(buf);
    }

    /* Session not running, try cache */
    if (read_cache(name) == 0) {
        return EXIT_SUCCESS_CODE;
    }

    /* Cache miss, try persistent log */
    if (read_log(name) == 0) {
        return EXIT_SUCCESS_CODE;
    }

    fprintf(stderr, "Error: Session '%s' not found\n", name);
    return EXIT_NOT_FOUND;
}

int cmd_wait(const char *name, const char *pattern, int timeout)
{
    ASSERT_MSG(name != NULL, "cmd_wait: name is NULL");
    ASSERT_MSG(pattern != NULL, "cmd_wait: pattern is NULL");
    ASSERT_MSG(timeout > 0, "cmd_wait: timeout must be positive, got %d", timeout);
    ASSERT_MSG(timeout <= 100000,
               "cmd_wait: timeout out of range: %d", timeout);

    if (name[0] == '\0' || pattern[0] == '\0') {
        fprintf(stderr, "Error: wait requires <name> and <pattern>\n");
        return EXIT_BAD_ARGS;
    }

    if (!is_safe_name(name)) {
        fprintf(stderr,
                "Error: Session name '%s' contains invalid characters "
                "(only [a-zA-Z0-9_-] allowed)\n", name);
        return EXIT_BAD_ARGS;
    }

    char session[MAX_SESSION_NAME];
    if (build_session_name(session, sizeof(session), name) < 0) {
        return EXIT_ERROR;
    }

    if (!session_exists(session)) {
        /* Session already gone — check persistent log for the pattern.
         * This handles the race where the session completes before
         * wait starts polling.
         * Violation S8 fix: use extracted helper. */
        int log_rc = search_log_for_pattern(name, pattern);
        if (log_rc == EXIT_SUCCESS_CODE) {
            return EXIT_SUCCESS_CODE;
        }
        fprintf(stderr, "Session '%s' exited without producing pattern '%s'\n",
                name, pattern);
        return EXIT_NOT_FOUND;
    }

    /* Violation S6 fix: use CLOCK_MONOTONIC for timeout */
    long timeout_ms = (long)timeout * 1000;
    /* Violation S7 fix: postcondition on multiplication */
    ASSERT_MSG(timeout_ms > 0 && timeout_ms / 1000 == (long)timeout,
               "timeout_ms overflow: timeout=%d, timeout_ms=%ld",
               timeout, timeout_ms);

    long start_ms = get_monotonic_ms();
    ASSERT_MSG(start_ms >= 0, "clock_gettime(CLOCK_MONOTONIC) failed");

    char *buf = malloc(CAPTURE_BUF_SIZE);
    if (!buf) {
        fprintf(stderr, "Error: malloc failed for %zu bytes (capture buffer)\n",
                (size_t)CAPTURE_BUF_SIZE);
        return EXIT_ERROR;
    }

    for (;;) {
        long now_ms = get_monotonic_ms();
        long elapsed_ms = (now_ms >= 0) ? (now_ms - start_ms) : timeout_ms;

        if (elapsed_ms >= timeout_ms) {
            fprintf(stderr, "Timeout after %ds waiting for pattern: %s\n",
                    timeout, pattern);
            free(buf);
            return EXIT_TIMEOUT;
        }

        /* Check if session is still alive */
        if (!session_exists(session)) {
            /* Session exited — check persistent log for the pattern.
             * Violation S8 fix: use extracted helper. */
            free(buf);
            int log_rc = search_log_for_pattern(name, pattern);
            if (log_rc == EXIT_SUCCESS_CODE) {
                return EXIT_SUCCESS_CODE;
            }
            fprintf(stderr, "Session '%s' exited without producing pattern '%s'\n",
                    name, pattern);
            return EXIT_NOT_FOUND;
        }

        if (capture_pane(session, DEFAULT_SCROLLBACK, buf, CAPTURE_BUF_SIZE) == 0) {
            if (strstr(buf, pattern) != NULL) {
                printf("Pattern found after %ld.%lds\n",
                       elapsed_ms / 1000, (elapsed_ms % 1000) / 100);
                free(buf);
                return EXIT_SUCCESS_CODE;
            }
        }

        usleep(POLL_INTERVAL_USEC);
    }
}

int cmd_kill(const char *name)
{
    ASSERT_MSG(name != NULL, "cmd_kill: name is NULL");

    if (name[0] == '\0') {
        fprintf(stderr, "Error: kill requires <name>\n");
        return EXIT_BAD_ARGS;
    }

    if (!is_safe_name(name)) {
        fprintf(stderr,
                "Error: Session name '%s' contains invalid characters "
                "(only [a-zA-Z0-9_-] allowed)\n", name);
        return EXIT_BAD_ARGS;
    }

    char session[MAX_SESSION_NAME];
    if (build_session_name(session, sizeof(session), name) < 0) {
        return EXIT_ERROR;
    }

    if (!session_exists(session)) {
        fprintf(stderr, "Error: Session '%s' not found\n", name);
        return EXIT_NOT_FOUND;
    }

    /* Cache session output before killing */
    if (cache_session(name, session) != 0) {
        fprintf(stderr, "Warning: Failed to cache session output\n");
    }

    /* tmux kill-session -t pty_<name> */
    const char *kill_argv[] = {
        "tmux", "kill-session", "-t", session, NULL
    };
    int rc = exec_fire_and_forget(kill_argv);
    if (rc != 0) {
        fprintf(stderr, "Error: Failed to kill session\n");
        return EXIT_ERROR;
    }

    printf("Killed session: %s\n", name);
    return EXIT_SUCCESS_CODE;
}

int cmd_list(void)
{
    printf("Active pty-session sessions:\n");

    int has_sessions = 0;
    int n;

    /* List running sessions */
    char *buf = malloc(CAPTURE_BUF_SIZE);
    if (!buf) {
        fprintf(stderr, "Error: malloc failed for %zu bytes (list buffer)\n",
                (size_t)CAPTURE_BUF_SIZE);
        return EXIT_ERROR;
    }

    const char *list_argv[] = {
        "tmux", "list-sessions", "-F", "#{session_name}", NULL
    };

    if (exec_capture(list_argv, buf, CAPTURE_BUF_SIZE) == 0) {
        /* Parse line by line, filter for pty_ prefix */
        char *line = buf;
        while (line && *line) {
            char *newline = strchr(line, '\n');
            if (newline) {
                *newline = '\0';
            }

            if (strncmp(line, PTY_PREFIX, PTY_PREFIX_LEN) == 0) {
                const char *sname = line + PTY_PREFIX_LEN;
                printf("  %-20s running\n", sname);
                has_sessions = 1;
            }

            if (newline) {
                line = newline + 1;
            } else {
                break;
            }
        }
    }

    /*
     * Track names already printed so we don't duplicate entries.
     * Violation S2 fix: use dynamic allocation instead of 64 KiB
     * stack array.
     */
    int seen_capacity = 256;
    int seen_count = 0;
    char (*seen_names)[MAX_SESSION_NAME] = malloc(
        (size_t)seen_capacity * sizeof(*seen_names));
    if (!seen_names) {
        fprintf(stderr, "Error: malloc failed for seen_names tracking\n");
        free(buf);
        return EXIT_ERROR;
    }

    /* List killed sessions from cache */
    char cache_dir[MAX_PATH_LEN];
    if (resolve_home_path(cache_dir, sizeof(cache_dir), ".pty-session/cache") == 0) {
        DIR *dir = opendir(cache_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                const char *dot = strrchr(entry->d_name, '.');
                if (dot && strcmp(dot, ".output") == 0) {
                    /* Extract name (everything before .output) */
                    char sname[MAX_SESSION_NAME];
                    size_t name_len = (size_t)(dot - entry->d_name);
                    if (name_len >= sizeof(sname)) {
                        name_len = sizeof(sname) - 1;
                    }
                    memcpy(sname, entry->d_name, name_len);
                    sname[name_len] = '\0';

                    printf("  %-20s killed (cached)\n", sname);
                    has_sessions = 1;

                    /* Violation S2 fix: grow buffer if needed */
                    if (seen_count >= seen_capacity) {
                        int new_cap = seen_capacity * 2;
                        char (*new_names)[MAX_SESSION_NAME] = realloc(
                            seen_names,
                            (size_t)new_cap * sizeof(*new_names));
                        if (!new_names) {
                            fprintf(stderr,
                                    "Error: realloc failed for seen_names "
                                    "(at %d entries)\n", seen_count);
                            /* Cannot maintain dedup invariant; abort listing */
                            closedir(dir);
                            free(seen_names);
                            free(buf);
                            return EXIT_ERROR;
                        }
                        seen_names = new_names;
                        seen_capacity = new_cap;
                    }

                    n = snprintf(seen_names[seen_count],
                                 sizeof(seen_names[0]), "%s", sname);
                    ASSERT_MSG(n >= 0 && (size_t)n < sizeof(seen_names[0]),
                               "seen_names truncated");
                    seen_count++;
                }
            }
            closedir(dir);
        }
    }

    /* List sessions with logs but no cache (exited naturally) */
    char log_dir[MAX_PATH_LEN];
    if (resolve_home_path(log_dir, sizeof(log_dir), ".pty-session/logs") == 0) {
        DIR *dir = opendir(log_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                const char *dot = strrchr(entry->d_name, '.');
                if (dot && strcmp(dot, ".log") == 0) {
                    char sname[MAX_SESSION_NAME];
                    size_t name_len = (size_t)(dot - entry->d_name);
                    if (name_len >= sizeof(sname)) {
                        name_len = sizeof(sname) - 1;
                    }
                    memcpy(sname, entry->d_name, name_len);
                    sname[name_len] = '\0';

                    /* Skip if already listed from cache */
                    int already_seen = 0;
                    for (int i = 0; i < seen_count; i++) {
                        if (strcmp(seen_names[i], sname) == 0) {
                            already_seen = 1;
                            break;
                        }
                    }

                    /* Skip if still running */
                    if (!already_seen) {
                        char session[MAX_SESSION_NAME];
                        if (build_session_name(session, sizeof(session), sname) == 0) {
                            if (session_exists(session)) {
                                already_seen = 1;
                            }
                        }
                    }

                    if (!already_seen) {
                        printf("  %-20s exited (log available)\n", sname);
                        has_sessions = 1;
                    }
                }
            }
            closedir(dir);
        }
    }

    if (!has_sessions) {
        printf("  (none)\n");
    }

    free(seen_names);
    free(buf);
    return EXIT_SUCCESS_CODE;
}

int cmd_help(void)
{
    printf(
        "pty-session: Manage interactive terminal sessions via tmux\n"
        "\n"
        "Usage:\n"
        "  pty-session create <name> <command>   Create session running command\n"
        "  pty-session send <name> <text>         Send keystrokes (adds Enter by default)\n"
        "  pty-session read <name>                Read session output (live, cache, or log)\n"
        "  pty-session wait <name> <pattern>      Poll until pattern appears (default 60s)\n"
        "  pty-session kill <name>                Terminate session (screen cached)\n"
        "  pty-session list                       Show active and killed pty-session sessions\n"
        "  pty-session help                       Show this help\n"
        "\n"
        "Options:\n"
        "  --no-enter     With 'send': don't append Enter after text\n"
        "  --timeout=N    With 'wait' or 'read --wait': timeout in seconds\n"
        "  --scrollback=N With 'read': lines of scrollback to capture (default 100)\n"
        "  --last=N       Alias for --scrollback=N\n"
        "  --wait         With 'read': block until session exits, then read cache\n"
        "\n"
        "Examples:\n"
        "  pty-session create myrepl 'python3'\n"
        "  pty-session send myrepl 'print(\"hello\")'\n"
        "  pty-session read myrepl\n"
        "  pty-session wait myrepl '>>>'\n"
        "  pty-session kill myrepl\n"
        "\n"
        "Exit codes:\n"
        "  0 - Success\n"
        "  1 - General error\n"
        "  2 - Session not found\n"
        "  3 - Timeout (for wait command)\n"
        "  4 - Invalid arguments\n"
    );

    return EXIT_SUCCESS_CODE;
}

/* ── Test-visible wrappers ────────────────────────────────────────── */

#ifdef TEST_BUILD

int test_is_safe_name(const char *name)
{
    return is_safe_name(name);
}

int test_is_safe_home_path(const char *path)
{
    return is_safe_home_path(path);
}

#endif /* TEST_BUILD */
