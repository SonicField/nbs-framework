/*
 * session.c — PTY session creation, child process management, cleanup.
 *
 * Each session creates:
 *   ~/.nbs-ts/sessions/<handle>/
 *   ├── output.log        Append-only stdout+stderr capture
 *   ├── completion.log    One line per command: seq exit_code
 *   ├── pid               Child process ID
 *   └── meta              Command, cwd, start time
 *
 * The output capture thread reads the PTY master fd and appends to
 * output.log with fsync after every write.
 *
 * Completion signalling: on creation, PROMPT_COMMAND is injected into
 * bash. After every command, bash appends "seq exit_code" to
 * completion.log. wait_complete watches this file with inotify.
 *
 * Invariants:
 *   - Child processes use _exit(), never exit()
 *   - All string formatting via snprintf (never sprintf/strcat)
 *   - Manual resource cleanup on all error paths
 *   - Session directories created with mode 0700
 */

/*
 * GCC's -Wformat-truncation computes worst-case buffer sizes from
 * declared array lengths. Our path buffers (8192 bytes) are far larger
 * than actual paths (~50 bytes), so truncation cannot occur in practice.
 * Disable this warning for the known-safe snprintf chains in this file.
 */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "session_internal.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

/* PTY headers */
#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#endif

/* ── Accessor functions for io.c and wait.c ───────────────────────── */

int nbs_ts_get_master_fd(const nbs_ts_session_t *s)  { return s->master_fd; }
const char *nbs_ts_get_output_log_path(const nbs_ts_session_t *s)  { return s->output_log_path; }
const char *nbs_ts_get_completion_log_path(const nbs_ts_session_t *s) { return s->completion_log_path; }
off_t nbs_ts_get_read_cursor(const nbs_ts_session_t *s)  { return s->read_cursor; }
void nbs_ts_set_read_cursor(nbs_ts_session_t *s, off_t pos)  { s->read_cursor = pos; }
unsigned long nbs_ts_get_completion_cursor(const nbs_ts_session_t *s)  { return s->completion_cursor; }
void nbs_ts_set_completion_cursor(nbs_ts_session_t *s, unsigned long seq)  { s->completion_cursor = seq; }

/* ── Utilities ────────────────────────────────────────────────────── */

static int generate_handle(char *buf, size_t bufsize)
{
    ASSERT_MSG(buf != NULL, "generate_handle: buf is NULL");
    ASSERT_MSG(bufsize >= 9, "generate_handle: bufsize too small: %zu", bufsize);

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;

    unsigned char bytes[4];
    ssize_t n = read(fd, bytes, sizeof(bytes));
    close(fd);

    if (n != (ssize_t)sizeof(bytes)) return -1;

    snprintf(buf, bufsize, "%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3]);
    return 0;
}

int nbs_ts_sessions_dir(char *buf, size_t bufsize)
{
    ASSERT_MSG(buf != NULL, "nbs_ts_sessions_dir: buf is NULL");

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        fprintf(stderr, "nbs-ts: HOME not set\n");
        return -1;
    }

    int n = snprintf(buf, bufsize, "%s/.nbs-ts/sessions", home);
    if (n < 0 || (size_t)n >= bufsize) return -1;

    char parent[NBS_TS_MAX_PATH];
    snprintf(parent, sizeof(parent), "%s/.nbs-ts", home);
    mkdir(parent, 0700);
    mkdir(buf, 0700);

    return 0;
}

int nbs_ts_session_dir(const char *handle, char *buf, size_t bufsize)
{
    ASSERT_MSG(handle != NULL, "nbs_ts_session_dir: handle is NULL");
    ASSERT_MSG(buf != NULL, "nbs_ts_session_dir: buf is NULL");

    char sessions[NBS_TS_MAX_PATH];
    if (nbs_ts_sessions_dir(sessions, sizeof(sessions)) < 0) return -1;

    int n = snprintf(buf, bufsize, "%s/%s", sessions, handle);
    if (n < 0 || (size_t)n >= bufsize) return -1;

    return 0;
}

static int write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;

    size_t len = strlen(content);
    ssize_t w = write(fd, content, len);
    fsync(fd);
    close(fd);

    return (w == (ssize_t)len) ? 0 : -1;
}

/* ── Output capture thread ────────────────────────────────────────── */

static void *capture_thread_fn(void *arg)
{
    nbs_ts_session_t *s = (nbs_ts_session_t *)arg;

    int log_fd = open(s->output_log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log_fd < 0) return NULL;

    char buf[4096];
    ssize_t n;

    while ((n = read(s->master_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(log_fd, buf + written, (size_t)(n - written));
            if (w < 0) {
                if (errno == EINTR) continue;
                goto done;
            }
            written += w;
        }
        fsync(log_fd);
    }

done:
    close(log_fd);
    return NULL;
}

/* ── Session lifecycle ────────────────────────────────────────────── */

nbs_ts_session_t *nbs_ts_create(const char *command, const nbs_ts_opts_t *opts)
{
    ASSERT_MSG(command != NULL, "nbs_ts_create: command is NULL");

    nbs_ts_session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->master_fd = -1;
    s->child_pid = -1;

    if (generate_handle(s->handle, sizeof(s->handle)) < 0) {
        free(s);
        return NULL;
    }

    char sessions_dir[NBS_TS_MAX_PATH];
    if (nbs_ts_sessions_dir(sessions_dir, sizeof(sessions_dir)) < 0) {
        free(s);
        return NULL;
    }

    snprintf(s->session_dir, sizeof(s->session_dir),
             "%s/%s", sessions_dir, s->handle);
    if (mkdir(s->session_dir, 0700) < 0) {
        free(s);
        return NULL;
    }

    snprintf(s->output_log_path, sizeof(s->output_log_path),
             "%s/output.log", s->session_dir);
    snprintf(s->completion_log_path, sizeof(s->completion_log_path),
             "%s/completion.log", s->session_dir);

    int out_fd = open(s->output_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd >= 0) close(out_fd);

    int comp_fd = open(s->completion_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (comp_fd >= 0) close(comp_fd);

    /* Create PTY */
    int slave_fd;
    struct winsize ws = { .ws_row = 24, .ws_col = 80 };

    if (openpty(&s->master_fd, &slave_fd, NULL, NULL, &ws) < 0) {
        unlink(s->output_log_path);
        unlink(s->completion_log_path);
        rmdir(s->session_dir);
        free(s);
        return NULL;
    }

    /* Fork child */
    s->child_pid = fork();
    if (s->child_pid < 0) {
        close(s->master_fd);
        close(slave_fd);
        unlink(s->output_log_path);
        unlink(s->completion_log_path);
        rmdir(s->session_dir);
        free(s);
        return NULL;
    }

    if (s->child_pid == 0) {
        /* ── Child process ── */
        close(s->master_fd);

        setsid();
        ioctl(slave_fd, TIOCSCTTY, 0);

        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) close(slave_fd);

        if (opts && opts->cwd) {
            if (chdir(opts->cwd) < 0) _exit(127);
        }

        unsetenv("TMUX");

        /*
         * Set up PROMPT_COMMAND for completion signalling.
         *
         * We use bash -c to execute the user's command wrapped with
         * PROMPT_COMMAND setup. For interactive shells (bare "bash"),
         * PROMPT_COMMAND fires after each command and writes seq+exit_code
         * to the completion log.
         *
         * For non-interactive commands, PROMPT_COMMAND won't fire, but
         * that's fine — the caller uses wait_pattern or checks exit code
         * via process status instead.
         */
        char setup[NBS_TS_MAX_FILE_PATH * 2];
        snprintf(setup, sizeof(setup),
                 "NBS_TS_SEQ=0; "
                 "PROMPT_COMMAND='NBS_TS_SEQ=$((NBS_TS_SEQ + 1)); "
                 "echo \"$NBS_TS_SEQ $?\" >> \"%s\"'; "
                 "exec %s",
                 s->completion_log_path, command);

        execlp("bash", "bash", "-c", setup, (char *)NULL);
        _exit(127);
    }

    /* ── Parent process ── */
    close(slave_fd);

    /* Write metadata files */
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", (int)s->child_pid);
    char pid_path[NBS_TS_MAX_FILE_PATH];
    snprintf(pid_path, sizeof(pid_path), "%s/pid", s->session_dir);
    write_file(pid_path, pid_str);

    char meta_path[NBS_TS_MAX_FILE_PATH];
    snprintf(meta_path, sizeof(meta_path), "%s/meta", s->session_dir);
    char meta_buf[NBS_TS_MAX_FILE_PATH];
    time_t now = time(NULL);
    snprintf(meta_buf, sizeof(meta_buf), "command: %s\nstart: %ld\n",
             command, (long)now);
    write_file(meta_path, meta_buf);

    /* Start capture thread */
    if (pthread_create(&s->capture_thread, NULL, capture_thread_fn, s) == 0) {
        s->capture_running = 1;
    }

    return s;
}

void nbs_ts_destroy(nbs_ts_session_t *s)
{
    if (!s) return;

    if (s->child_pid > 0 && !s->child_exited) {
        kill(s->child_pid, SIGTERM);
        int status;
        int tries = 0;
        while (tries < 10) {
            pid_t r = waitpid(s->child_pid, &status, WNOHANG);
            if (r == s->child_pid || r < 0) break;
            usleep(10000);
            tries++;
        }
        if (tries >= 10) {
            kill(s->child_pid, SIGKILL);
            waitpid(s->child_pid, &status, 0);
        }
    }

    if (s->master_fd >= 0) {
        close(s->master_fd);
        s->master_fd = -1;
    }

    if (s->capture_running) {
        pthread_join(s->capture_thread, NULL);
        s->capture_running = 0;
    }

    char path[NBS_TS_MAX_FILE_PATH];
    snprintf(path, sizeof(path), "%s/output.log", s->session_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/completion.log", s->session_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/pid", s->session_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/meta", s->session_dir);
    unlink(path);

    rmdir(s->session_dir);
    free(s);
}

/* ── Status queries ───────────────────────────────────────────────── */

nbs_ts_status_t nbs_ts_status(nbs_ts_session_t *s)
{
    ASSERT_MSG(s != NULL, "nbs_ts_status: session is NULL");

    if (s->child_exited) return NBS_TS_DEAD;
    if (s->child_pid <= 0) return NBS_TS_UNKNOWN;

    int status;
    pid_t r = waitpid(s->child_pid, &status, WNOHANG);

    if (r == 0) return NBS_TS_ALIVE;

    if (r == s->child_pid) {
        s->child_exited = 1;
        if (WIFEXITED(status)) {
            s->child_exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            s->child_exit_code = 128 + WTERMSIG(status);
        } else {
            s->child_exit_code = 1;
        }
        return NBS_TS_DEAD;
    }

    return NBS_TS_UNKNOWN;
}

int nbs_ts_exit_code(nbs_ts_session_t *s)
{
    ASSERT_MSG(s != NULL, "nbs_ts_exit_code: session is NULL");

    if (!s->child_exited) nbs_ts_status(s);
    return s->child_exited ? s->child_exit_code : -1;
}

const char *nbs_ts_handle(const nbs_ts_session_t *s)
{
    ASSERT_MSG(s != NULL, "nbs_ts_handle: session is NULL");
    return s->handle;
}

pid_t nbs_ts_pid(const nbs_ts_session_t *s)
{
    ASSERT_MSG(s != NULL, "nbs_ts_pid: session is NULL");
    return s->child_pid;
}
