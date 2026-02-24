/*
 * worker.c — Worker lifecycle management implementation.
 *
 * Implements all 8 nbs-workers commands: spawn, status, search, results,
 * dismiss, continue, session, list, plus help.
 *
 * All external commands (tmux, nbs-bus, nbs-claude) are invoked via
 * fork+exec — never system().
 *
 * Invariants:
 *   - Name validation (path traversal defence) on every command that
 *     takes a worker name
 *   - snprintf only (no sprintf, no unbounded writes)
 *   - Child processes always _exit(), never exit()
 *   - Bus integration is graceful (no-op if nbs-bus not found)
 */

#include "worker.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * redirect_stderr_to_devnull — Redirect stderr to /dev/null in child.
 */
static void redirect_stderr_to_devnull(void)
{
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
        if (dup2(fd, STDERR_FILENO) < 0)
            _exit(126);
        close(fd);
    } else {
        close(STDERR_FILENO);
    }
}

/*
 * exec_capture — Fork+exec a command, capture stdout to buffer.
 *
 * Returns child exit code on success, -1 on fork/exec failure.
 * out_buf is always NUL-terminated on success.
 */
static int exec_capture(const char *const argv[], char *out_buf, size_t out_size)
{
    ASSERT_MSG(argv != NULL && argv[0] != NULL,
               "exec_capture: argv or argv[0] is NULL");
    ASSERT_MSG(out_buf != NULL, "exec_capture: out_buf is NULL");
    ASSERT_MSG(out_size > 0, "exec_capture: out_size is 0");

    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(pipefd[1]);
        redirect_stderr_to_devnull();
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent */
    close(pipefd[1]);

    size_t total = 0;
    while (total < out_size - 1) {
        ssize_t n = read(pipefd[0], out_buf + total, out_size - 1 - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
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

    if (wpid < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

/*
 * exec_fire_and_forget — Fork+exec without capturing output.
 *
 * Returns child exit code, or -1 on failure.
 */
static int exec_fire_and_forget(const char *const argv[])
{
    ASSERT_MSG(argv != NULL && argv[0] != NULL,
               "exec_fire_and_forget: argv or argv[0] is NULL");

    pid_t pid = fork();
    if (pid < 0)
        return -1;

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

    if (wpid < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

/*
 * exec_spawn_detached — Fork+exec a command, wait for the client process
 * to exit (used for tmux new-session -d which detaches and exits).
 *
 * Returns 0 on success, -1 on failure.
 */
static int exec_spawn_detached(const char *const argv[])
{
    ASSERT_MSG(argv != NULL && argv[0] != NULL,
               "exec_spawn_detached: argv or argv[0] is NULL");

    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        redirect_stderr_to_devnull();
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            if (dup2(fd, STDOUT_FILENO) < 0)
                _exit(126);
            close(fd);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    int status;
    pid_t wpid;
    do {
        wpid = waitpid(pid, &status, 0);
    } while (wpid < 0 && errno == EINTR);

    if (wpid < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status) == 0 ? 0 : -1;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Path construction helpers                                           */
/* ------------------------------------------------------------------ */

static void build_task_file_path(char *buf, size_t bufsz,
                                 const char *cwd, const char *name)
{
    int n = snprintf(buf, bufsz, "%s/%s/%s.md", cwd, WORKERS_SUBDIR, name);
    ASSERT_MSG(n > 0 && (size_t)n < bufsz,
               "build_task_file_path: path too long");
}

static void build_log_file_path(char *buf, size_t bufsz,
                                const char *cwd, const char *name)
{
    int n = snprintf(buf, bufsz, "%s/%s/%s.log", cwd, WORKERS_SUBDIR, name);
    ASSERT_MSG(n > 0 && (size_t)n < bufsz,
               "build_log_file_path: path too long");
}

static void build_workers_dir(char *buf, size_t bufsz, const char *cwd)
{
    int n = snprintf(buf, bufsz, "%s/%s", cwd, WORKERS_SUBDIR);
    ASSERT_MSG(n > 0 && (size_t)n < bufsz,
               "build_workers_dir: path too long");
}

static void build_session_file_path(char *buf, size_t bufsz,
                                    const char *cwd, const char *handle)
{
    int n = snprintf(buf, bufsz, "%s/%s/%s.json", cwd, SESSIONS_SUBDIR, handle);
    ASSERT_MSG(n > 0 && (size_t)n < bufsz,
               "build_session_file_path: path too long");
}

static void build_events_dir(char *buf, size_t bufsz, const char *cwd)
{
    int n = snprintf(buf, bufsz, "%s/%s", cwd, EVENTS_SUBDIR);
    ASSERT_MSG(n > 0 && (size_t)n < bufsz,
               "build_events_dir: path too long");
}

static void build_session_name(char *buf, size_t bufsz, const char *name)
{
    int n = snprintf(buf, bufsz, "%s%s", TMUX_PREFIX, name);
    ASSERT_MSG(n > 0 && (size_t)n < bufsz,
               "build_session_name: name too long");
}

/* ------------------------------------------------------------------ */
/* Timestamp helper                                                    */
/* ------------------------------------------------------------------ */

static void get_timestamp(char *buf, size_t bufsz)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    ASSERT_MSG(tm != NULL, "localtime returned NULL");
    size_t n = strftime(buf, bufsz, "%Y-%m-%d %H:%M:%S", tm);
    ASSERT_MSG(n > 0, "strftime failed");
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

int validate_slug(const char *slug)
{
    ASSERT_MSG(slug != NULL, "validate_slug: slug is NULL");
    if (slug == NULL || slug[0] == '\0')
        return 0;
    for (const char *p = slug; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
            return 0;
    }
    return 1;
}

int validate_worker_name(const char *name)
{
    ASSERT_MSG(name != NULL, "validate_worker_name: name is NULL");
    if (name == NULL || name[0] == '\0')
        return 0;

    /* Find the last dash — everything before is slug, after is 4 hex chars */
    const char *dash = strrchr(name, '-');
    if (dash == NULL || dash == name)
        return 0;

    /* Validate slug portion: [a-z0-9]+ */
    for (const char *p = name; p < dash; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
            return 0;
    }

    /* Validate hex suffix: exactly 4 hex chars */
    const char *hex = dash + 1;
    if (strlen(hex) != 4)
        return 0;
    for (int i = 0; i < 4; i++) {
        char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }

    return 1;
}

int validate_uuid(const char *s)
{
    ASSERT_MSG(s != NULL, "validate_uuid: s is NULL");
    /* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars) */
    if (s == NULL || strlen(s) != 36)
        return 0;

    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return 0;
        } else {
            char c = s[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* File I/O helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * read_file — Read entire file into malloc'd buffer. Caller must free.
 * Returns NULL on failure.
 */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, (size_t)sz, f);
    buf[nread] = '\0';
    fclose(f);

    ASSERT_MSG(buf[nread] == '\0', "read_file: buffer not NUL-terminated");

    if (out_len)
        *out_len = nread;
    return buf;
}

static int file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

static int dir_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* ------------------------------------------------------------------ */
/* tmux helpers                                                        */
/* ------------------------------------------------------------------ */

static int tmux_has_session(const char *session_name)
{
    const char *argv[] = {"tmux", "has-session", "-t", session_name, NULL};
    char buf[64];
    int rc = exec_capture(argv, buf, sizeof(buf));
    return (rc == 0);
}

static int tmux_kill_session(const char *session_name)
{
    const char *argv[] = {"tmux", "kill-session", "-t", session_name, NULL};
    return exec_fire_and_forget(argv);
}

static int tmux_send_keys(const char *session_name, const char *keys,
                          int send_enter)
{
    if (send_enter) {
        const char *argv[] = {"tmux", "send-keys", "-t", session_name,
                              keys, "Enter", NULL};
        return exec_fire_and_forget(argv);
    }
    const char *argv[] = {"tmux", "send-keys", "-t", session_name,
                          keys, NULL};
    return exec_fire_and_forget(argv);
}

static int tmux_capture_pane(const char *session_name, char *buf, size_t bufsz)
{
    const char *argv[] = {"tmux", "capture-pane", "-t", session_name,
                          "-p", NULL};
    return exec_capture(argv, buf, bufsz);
}

static int tmux_pipe_pane(const char *session_name, const char *cmd)
{
    const char *argv[] = {"tmux", "pipe-pane", "-t", session_name,
                          "-o", cmd, NULL};
    return exec_fire_and_forget(argv);
}

/* ------------------------------------------------------------------ */
/* Bus integration                                                     */
/* ------------------------------------------------------------------ */

static void bus_publish(const char *cwd, const char *source,
                        const char *type, const char *priority,
                        const char *payload)
{
    char events_dir[PATH_BUF_SIZE];
    build_events_dir(events_dir, sizeof(events_dir), cwd);

    if (!dir_exists(events_dir))
        return;

    /* Try bin/nbs-bus first, then PATH */
    char nbs_bus_path[PATH_BUF_SIZE];
    snprintf(nbs_bus_path, sizeof(nbs_bus_path), "%s/bin/nbs-bus", cwd);

    const char *nbs_bus = NULL;
    if (access(nbs_bus_path, X_OK) == 0) {
        nbs_bus = nbs_bus_path;
    } else {
        /* Check PATH via which */
        char which_buf[PATH_BUF_SIZE];
        const char *which_argv[] = {"which", "nbs-bus", NULL};
        int rc = exec_capture(which_argv, which_buf, sizeof(which_buf));
        if (rc == 0 && which_buf[0] != '\0') {
            size_t len = strlen(which_buf);
            if (len > 0 && which_buf[len - 1] == '\n')
                which_buf[len - 1] = '\0';
            /* which_buf is on the stack — copy to nbs_bus_path for lifetime */
            snprintf(nbs_bus_path, sizeof(nbs_bus_path), "%s", which_buf);
            nbs_bus = nbs_bus_path;
        }
    }

    if (nbs_bus == NULL)
        return;

    if (payload != NULL && payload[0] != '\0') {
        const char *argv[] = {nbs_bus, "publish", events_dir, source,
                              type, priority, payload, NULL};
        if (exec_fire_and_forget(argv) != 0) {
            fprintf(stderr, "Warning: bus publish failed (source=%s type=%s)\n",
                    source, type);
        }
    } else {
        const char *argv[] = {nbs_bus, "publish", events_dir, source,
                              type, priority, NULL};
        if (exec_fire_and_forget(argv) != 0) {
            fprintf(stderr, "Warning: bus publish failed (source=%s type=%s)\n",
                    source, type);
        }
    }
}

/* ------------------------------------------------------------------ */
/* State field extraction                                              */
/* ------------------------------------------------------------------ */

static void get_state_field(const char *task_file_path, char *buf, size_t bufsz)
{
    ASSERT_MSG(task_file_path != NULL, "get_state_field: path is NULL");
    ASSERT_MSG(buf != NULL, "get_state_field: buf is NULL");
    ASSERT_MSG(bufsz > 0, "get_state_field: bufsz is 0");
    FILE *f = fopen(task_file_path, "r");
    if (!f) {
        snprintf(buf, bufsz, "not found");
        return;
    }

    char line[LINE_BUF_SIZE];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "State:", 6) == 0) {
            const char *val = line + 6;
            while (*val == ' ' || *val == '\t')
                val++;
            size_t vlen = strlen(val);
            while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r'
                                || val[vlen - 1] == ' '))
                vlen--;
            if (vlen == 0) {
                snprintf(buf, bufsz, "unknown");
            } else {
                size_t copy_len = vlen < bufsz - 1 ? vlen : bufsz - 1;
                memcpy(buf, val, copy_len);
                buf[copy_len] = '\0';
            }
            fclose(f);
            return;
        }
    }

    fclose(f);
    snprintf(buf, bufsz, "unknown");
}

/* ------------------------------------------------------------------ */
/* Name generation                                                     */
/* ------------------------------------------------------------------ */

static int generate_name(const char *slug, char *buf, size_t bufsz)
{
    ASSERT_MSG(slug != NULL && slug[0] != '\0', "generate_name: slug is empty");
    ASSERT_MSG(bufsz >= NAME_MAX_LEN, "generate_name: buffer too small");

    unsigned char randbuf[32];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open /dev/urandom\n");
        return -1;
    }

    ssize_t nread = 0;
    while (nread < (ssize_t)sizeof(randbuf)) {
        ssize_t n = read(fd, randbuf + nread, sizeof(randbuf) - (size_t)nread);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            fprintf(stderr, "Error: failed to read /dev/urandom\n");
            return -1;
        }
        nread += n;
    }
    close(fd);

    /* XOR-fold 32 bytes down to 2 bytes (4 hex chars) */
    unsigned char h[2] = {0, 0};
    for (int i = 0; i < 32; i++) {
        h[i % 2] ^= randbuf[i];
    }

    int n = snprintf(buf, bufsz, "%s-%02x%02x", slug, h[0], h[1]);
    if (n < 0 || (size_t)n >= bufsz)
        return -1;

    /* Postcondition: generated name is valid */
    if (!validate_worker_name(buf)) {
        fprintf(stderr, "ASSERTION FAILED: generate_name produced invalid name: %s\n", buf);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* ANSI stripping                                                      */
/* ------------------------------------------------------------------ */

static size_t strip_ansi(const char *input, size_t input_len,
                         char *output, size_t output_size)
{
    enum { NORMAL, ESC_SEEN, CSI, OSC, CHARSET } state = NORMAL;
    size_t out_pos = 0;

    for (size_t i = 0; i < input_len && out_pos < output_size - 1; i++) {
        unsigned char c = (unsigned char)input[i];

        switch (state) {
        case NORMAL:
            if (c == 0x1b) {
                state = ESC_SEEN;
            } else {
                output[out_pos++] = (char)c;
            }
            break;

        case ESC_SEEN:
            if (c == '[') {
                state = CSI;
            } else if (c == ']') {
                state = OSC;
            } else if (c == '(' || c == ')') {
                state = CHARSET;
            } else {
                /* Unknown ESC sequence — skip ESC and this char */
                state = NORMAL;
            }
            break;

        case CSI:
            if ((c >= '0' && c <= '9') || c == ';') {
                /* parameter bytes — skip */
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                state = NORMAL;
            }
            /* else: intermediate bytes — continue consuming */
            break;

        case OSC:
            if (c == 0x07) {
                state = NORMAL;
            } else if (c == 0x1b) {
                state = ESC_SEEN;
            }
            break;

        case CHARSET:
            /* One character after ESC( or ESC) — skip it */
            state = NORMAL;
            break;
        }
    }

    output[out_pos] = '\0';
    return out_pos;
}

/* ------------------------------------------------------------------ */
/* JSON field extraction (minimal, for fixed-format session files)      */
/* ------------------------------------------------------------------ */

static int json_extract_string(const char *json, const char *key,
                               char *buf, size_t bufsz)
{
    char pattern[256];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pattern))
        return -1;

    const char *pos = strstr(json, pattern);
    if (!pos)
        return -1;

    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')
        pos++;
    if (*pos != ':')
        return -1;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')
        pos++;

    if (*pos != '"')
        return -1;
    pos++;

    size_t i = 0;
    while (*pos && *pos != '"' && i < bufsz - 1) {
        buf[i++] = *pos++;
    }
    buf[i] = '\0';

    return 0;
}

static long json_extract_number(const char *json, const char *key)
{
    char pattern[256];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pattern))
        return -1;

    const char *pos = strstr(json, pattern);
    if (!pos)
        return -1;

    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')
        pos++;
    if (*pos != ':')
        return -1;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')
        pos++;

    char *end = NULL;
    long val = strtol(pos, &end, 10);
    if (end == pos)
        return -1;
    return val;
}

/* ------------------------------------------------------------------ */
/* In-place field update                                               */
/* ------------------------------------------------------------------ */

static int update_field_in_file(const char *path, const char *prefix,
                                const char *new_value)
{
    size_t file_len = 0;
    char *content = read_file(path, &file_len);
    if (!content)
        return -1;

    char *line_start = strstr(content, prefix);
    if (!line_start) {
        free(content);
        return -1;
    }

    char *line_end = strchr(line_start, '\n');
    if (!line_end)
        line_end = content + file_len;

    size_t before_len = (size_t)(line_start - content);
    size_t prefix_len = strlen(prefix);
    size_t new_val_len = strlen(new_value);
    size_t after_len = file_len - (size_t)(line_end - content);

    size_t new_line_len = prefix_len + 1 + new_val_len; /* +1 for space */
    size_t new_total = before_len + new_line_len + after_len;

    char *new_content = malloc(new_total + 1);
    if (!new_content) {
        free(content);
        return -1;
    }

    memcpy(new_content, content, before_len);
    memcpy(new_content + before_len, prefix, prefix_len);
    new_content[before_len + prefix_len] = ' ';
    memcpy(new_content + before_len + prefix_len + 1, new_value, new_val_len);
    memcpy(new_content + before_len + new_line_len, line_end, after_len);
    new_content[new_total] = '\0';

    FILE *f = fopen(path, "w");
    if (!f) {
        free(content);
        free(new_content);
        return -1;
    }
    fwrite(new_content, 1, new_total, f);
    fclose(f);

    free(content);
    free(new_content);
    return 0;
}

/* ------------------------------------------------------------------ */
/* mkdir -p helper                                                     */
/* ------------------------------------------------------------------ */

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_BUF_SIZE];
    size_t len = strlen(path);
    if (len >= sizeof(tmp))
        return -1;

    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    return mkdir(tmp, mode) != 0 && errno != EEXIST ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Resolve absolute path                                               */
/* ------------------------------------------------------------------ */

static int resolve_absolute_path(const char *input, char *output, size_t outsize)
{
    char *resolved = realpath(input, NULL);
    if (!resolved)
        return -1;
    size_t len = strlen(resolved);
    if (len >= outsize) {
        free(resolved);
        return -1;
    }
    memcpy(output, resolved, len + 1);
    free(resolved);
    return 0;
}

/* ================================================================== */
/* Command implementations                                             */
/* ================================================================== */

/* --- help --- */

void cmd_help(void)
{
    printf(
        "nbs-workers: Worker lifecycle management for NBS teams\n"
        "\n"
        "Usage:\n"
        "  nbs-workers spawn <slug> <project-dir> <task-description>\n"
        "      Create task file, start Claude worker, send initial prompt.\n"
        "      Returns the generated worker name (e.g., parser-a3f1).\n"
        "\n"
        "  nbs-workers status <name>\n"
        "      Report worker status from tmux session and task file State field.\n"
        "\n"
        "  nbs-workers search <name> <regex> [--context=N]\n"
        "      Search persistent log for regex matches with context (default 50).\n"
        "\n"
        "  nbs-workers results <name>\n"
        "      Extract Log section from completed task file.\n"
        "\n"
        "  nbs-workers dismiss <name>\n"
        "      Kill tmux session, mark task file as dismissed.\n"
        "\n"
        "  nbs-workers continue <handle> [--model=MODEL]\n"
        "      Resume an agent from its session metadata. Kills old tmux\n"
        "      session, respawns with the saved session ID via claude --resume.\n"
        "      Optionally override the model.\n"
        "\n"
        "  nbs-workers session <handle>\n"
        "      Display session metadata (session ID, model, PID, status).\n"
        "\n"
        "  nbs-workers list\n"
        "      Show all workers with status summary.\n"
        "\n"
        "  nbs-workers help\n"
        "      Show this help.\n"
        "\n"
        "Exit codes:\n"
        "  0 - Success\n"
        "  1 - General error\n"
        "  2 - Worker not found\n"
        "  4 - Invalid arguments\n"
    );
}

/* --- spawn --- */

int cmd_spawn(const char *slug, const char *project_dir,
              const char *task_description, const char *cwd)
{
    (void)cwd; /* spawn uses project_dir as its anchor */

    if (!slug || !project_dir || !task_description ||
        slug[0] == '\0' || project_dir[0] == '\0' ||
        task_description[0] == '\0') {
        fprintf(stderr,
                "Error: spawn requires <slug> <project-dir> <task-description>\n"
                "Usage: nbs-workers spawn <slug> <project-dir> <task-description>\n");
        return EXIT_BAD_ARGS;
    }

    if (!validate_slug(slug)) {
        fprintf(stderr, "Error: slug must match ^[a-z0-9]+$ (got: %s)\n", slug);
        return EXIT_BAD_ARGS;
    }

    if (!dir_exists(project_dir)) {
        fprintf(stderr, "Error: Project directory not found: %s\n", project_dir);
        return EXIT_ERROR;
    }

    /* Resolve to absolute path */
    char abs_project_dir[PATH_BUF_SIZE];
    if (resolve_absolute_path(project_dir, abs_project_dir,
                              sizeof(abs_project_dir)) != 0) {
        fprintf(stderr, "Error: failed to resolve project directory: %s\n",
                project_dir);
        return EXIT_ERROR;
    }

    /* Workers directory is relative to project */
    char workers_dir[PATH_BUF_SIZE];
    snprintf(workers_dir, sizeof(workers_dir), "%s/%s",
             abs_project_dir, WORKERS_SUBDIR);

    if (mkdir_p(workers_dir, 0755) != 0) {
        fprintf(stderr, "Error: failed to create workers directory: %s\n",
                workers_dir);
        return EXIT_ERROR;
    }

    /* Generate unique name */
    char name[NAME_MAX_LEN];
    if (generate_name(slug, name, sizeof(name)) != 0) {
        fprintf(stderr, "Error: failed to generate worker name\n");
        return EXIT_ERROR;
    }

    char task_file[PATH_BUF_SIZE];
    snprintf(task_file, sizeof(task_file), "%s/%s.md", workers_dir, name);

    char log_file[PATH_BUF_SIZE];
    snprintf(log_file, sizeof(log_file), "%s/%s.log", workers_dir, name);

    char session[NAME_MAX_LEN];
    build_session_name(session, sizeof(session), name);

    /* Precondition: no name collision */
    if (file_exists(task_file)) {
        fprintf(stderr,
                "Error: Name collision for '%s'. This should be extremely rare.\n",
                name);
        return EXIT_ERROR;
    }

    /* Create task file */
    char timestamp[TIMESTAMP_SIZE];
    get_timestamp(timestamp, sizeof(timestamp));

    FILE *f = fopen(task_file, "w");
    if (!f) {
        fprintf(stderr, "Error: failed to create task file: %s\n", task_file);
        return EXIT_ERROR;
    }

    fprintf(f,
        "# Worker: %s\n"
        "\n"
        "## Task\n"
        "\n"
        "%s\n"
        "\n"
        "## Tooling\n"
        "\n"
        "Your supervisor monitors you via `nbs-workers`. These tips avoid common mistakes:\n"
        "\n"
        "- **Do not read raw .log files** — they contain ANSI escape codes. "
        "Use `nbs-workers search <name> <regex>` for clean, searchable output.\n"
        "- **Update Status and Log sections** in this file when done — your "
        "supervisor reads them via `nbs-workers results`.\n"
        "- **Escalate blockers** by setting State to `escalated` — do not "
        "work around problems silently.\n"
        "\n"
        "## Status\n"
        "\n"
        "State: running\n"
        "Started: %s\n"
        "Completed:\n"
        "\n"
        "## Log\n"
        "\n"
        "[Worker appends findings here]\n",
        slug, task_description, timestamp);

    fclose(f);

    /* Postcondition: task file exists */
    ASSERT_MSG(file_exists(task_file), "task file not created: %s", task_file);

    /* Create tmux session */
    char shell_cmd[PATH_BUF_SIZE];
    snprintf(shell_cmd, sizeof(shell_cmd), "cd '%s' && exec bash -l",
             abs_project_dir);

    {
        const char *argv[] = {"tmux", "new-session", "-d", "-s", session,
                              shell_cmd, NULL};
        if (exec_spawn_detached(argv) != 0) {
            fprintf(stderr, "Error: Failed to create tmux session\n");
            unlink(task_file);
            return EXIT_ERROR;
        }
    }

    /* Start persistent logging via pipe-pane */
    {
        char pipe_cmd[PATH_BUF_SIZE + 16];
        snprintf(pipe_cmd, sizeof(pipe_cmd), "cat >> '%s'", log_file);
        if (tmux_pipe_pane(session, pipe_cmd) != 0) {
            fprintf(stderr,
                    "Warning: pipe-pane failed — log capture may not work\n");
        }
    }

    /* Postcondition: tmux session is alive */
    if (!tmux_has_session(session)) {
        fprintf(stderr,
                "Error: tmux session died immediately after creation\n");
        unlink(task_file);
        return EXIT_ERROR;
    }

    /* Allow shell to initialise */
    sleep(2);

    /* Send the initial Claude command with NBS_HANDLE for sidecar identity */
    {
        char launch_cmd[PATH_BUF_SIZE];
        snprintf(launch_cmd, sizeof(launch_cmd),
                 "NBS_HANDLE=%s bin/nbs-claude --dangerously-skip-permissions",
                 name);
        tmux_send_keys(session, launch_cmd, 1);
    }

    /* Allow Claude to start */
    sleep(3);

    /* Send the task prompt */
    {
        char prompt[PATH_BUF_SIZE * 2];
        snprintf(prompt, sizeof(prompt),
                 "Read %s and execute the task. "
                 "Update the Status and Log sections when complete.",
                 task_file);
        tmux_send_keys(session, prompt, 1);
    }

    /* Wait for Claude Code UI to be ready (poll for prompt indicator) */
    {
        int wait_count = 0;
        int max_wait = 30;
        char pane_buf[CAPTURE_BUF_SIZE];

        while (wait_count < max_wait) {
            if (tmux_capture_pane(session, pane_buf, sizeof(pane_buf)) == 0) {
                /* UTF-8 encoded ❯ is 0xe2 0x9d 0xaf */
                if (strstr(pane_buf, "\xe2\x9d\xaf") != NULL) {
                    break;
                }
            }
            sleep(1);
            wait_count++;
        }

        if (wait_count >= max_wait) {
            fprintf(stderr,
                    "Warning: timed out waiting for Claude UI prompt (%ds)\n",
                    max_wait);
        }
    }

    /* Extra Enter for reliability */
    sleep(1);
    tmux_send_keys(session, "", 1);

    /* Publish bus event */
    {
        char payload[PATH_BUF_SIZE];
        snprintf(payload, sizeof(payload), "Task: %s", task_description);
        bus_publish(abs_project_dir, name, "worker-spawned", "normal", payload);
    }

    /* Output name to stdout, details to stderr */
    printf("%s\n", name);
    fprintf(stderr, "  project:   %s\n", abs_project_dir);
    fprintf(stderr, "  task file:  %s\n", task_file);
    fprintf(stderr, "  log file:   %s\n", log_file);
    fprintf(stderr, "  tmux:       %s\n", session);
    fprintf(stderr, "  note: run nbs-workers commands from %s\n",
            abs_project_dir);

    return EXIT_SUCCESS_CODE;
}

/* --- status --- */

int cmd_status(const char *name, const char *cwd)
{
    if (!name || name[0] == '\0') {
        fprintf(stderr, "Error: status requires <name>\n");
        return EXIT_BAD_ARGS;
    }

    char task_file[PATH_BUF_SIZE];
    build_task_file_path(task_file, sizeof(task_file), cwd, name);

    if (!file_exists(task_file)) {
        fprintf(stderr, "Error: Worker '%s' not found\n", name);
        return EXIT_NOT_FOUND;
    }

    char session[NAME_MAX_LEN];
    build_session_name(session, sizeof(session), name);

    int alive = tmux_has_session(session);

    char state[64];
    get_state_field(task_file, state, sizeof(state));

    /* Status truth table */
    const char *reported_status;
    if (strcmp(state, "not found") == 0 || strcmp(state, "unknown") == 0) {
        reported_status = "unknown";
    } else if (alive && strcmp(state, "running") == 0) {
        reported_status = "running";
    } else if (alive && strcmp(state, "completed") == 0) {
        reported_status = "completed (session still open)";
    } else if (!alive && strcmp(state, "running") == 0) {
        reported_status = "died (session exited unexpectedly)";
    } else if (!alive && strcmp(state, "completed") == 0) {
        reported_status = "completed";
    } else if (!alive) {
        reported_status = state;
    } else {
        reported_status = state;
    }

    printf("Worker: %s\n", name);
    printf("  tmux session: %s\n", alive ? "yes" : "no");
    printf("  task state:   %s\n", state);
    printf("  status:       %s\n", reported_status);

    if (strcmp(reported_status, "died (session exited unexpectedly)") == 0) {
        char payload[PATH_BUF_SIZE];
        snprintf(payload, sizeof(payload),
                 "Worker %s: tmux dead but state still running", name);
        bus_publish(cwd, name, "worker-died", "high", payload);
    }

    return EXIT_SUCCESS_CODE;
}

/* --- search --- */

int cmd_search(const char *name, const char *pattern,
               int context_lines, const char *cwd)
{
    if (!name || name[0] == '\0' || !pattern || pattern[0] == '\0') {
        fprintf(stderr,
                "Error: search requires <name> <regex> [--context=N]\n");
        return EXIT_BAD_ARGS;
    }

    /* Path traversal defence */
    if (!validate_worker_name(name)) {
        fprintf(stderr,
                "Error: invalid worker name format: %s "
                "(expected <slug>-<4hex>)\n", name);
        return EXIT_BAD_ARGS;
    }

    char log_file[PATH_BUF_SIZE];
    build_log_file_path(log_file, sizeof(log_file), cwd, name);

    if (!file_exists(log_file)) {
        fprintf(stderr, "Error: No log file found for worker '%s'\n", name);
        fprintf(stderr, "Log file expected at: %s\n", log_file);
        return EXIT_NOT_FOUND;
    }

    /* Validate regex with a dry-run */
    {
        const char *argv[] = {"grep", "-E", pattern, "/dev/null", NULL};
        char dummy[64];
        int rc = exec_capture(argv, dummy, sizeof(dummy));
        if (rc == 2) {
            fprintf(stderr, "Error: invalid regex pattern: %s\n", pattern);
            return EXIT_BAD_ARGS;
        }
    }

    /* Read log file */
    size_t log_len = 0;
    char *log_content = read_file(log_file, &log_len);
    if (!log_content) {
        fprintf(stderr, "Error: failed to read log file: %s\n", log_file);
        return EXIT_ERROR;
    }

    /* Strip ANSI codes */
    char *cleaned = malloc(log_len + 1);
    if (!cleaned) {
        free(log_content);
        fprintf(stderr, "Error: out of memory\n");
        return EXIT_ERROR;
    }
    strip_ansi(log_content, log_len, cleaned, log_len + 1);
    free(log_content);

    /* Pipe cleaned content to grep -E -C context_lines pattern */
    char context_str[32];
    snprintf(context_str, sizeof(context_str), "%d", context_lines);

    int pipefd_in[2];   /* parent writes, child reads */
    int pipefd_out[2];  /* child writes, parent reads */

    if (pipe(pipefd_in) < 0 || pipe(pipefd_out) < 0) {
        free(cleaned);
        fprintf(stderr, "Error: pipe() failed\n");
        return EXIT_ERROR;
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(cleaned);
        close(pipefd_in[0]);
        close(pipefd_in[1]);
        close(pipefd_out[0]);
        close(pipefd_out[1]);
        fprintf(stderr, "Error: fork() failed\n");
        return EXIT_ERROR;
    }

    if (pid == 0) {
        /* Child: grep -E -C context pattern */
        close(pipefd_in[1]);
        close(pipefd_out[0]);

        if (dup2(pipefd_in[0], STDIN_FILENO) < 0)
            _exit(126);
        close(pipefd_in[0]);
        if (dup2(pipefd_out[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(pipefd_out[1]);
        redirect_stderr_to_devnull();

        execlp("grep", "grep", "-E", "-C", context_str, "--", pattern,
               (char *)NULL);
        _exit(127);
    }

    /* Parent */
    close(pipefd_in[0]);
    close(pipefd_out[1]);

    /* Write cleaned content to grep's stdin */
    size_t cleaned_len = strlen(cleaned);
    size_t written = 0;
    while (written < cleaned_len) {
        ssize_t n = write(pipefd_in[1], cleaned + written,
                          cleaned_len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        written += (size_t)n;
    }
    close(pipefd_in[1]);
    free(cleaned);

    /* Read grep's output and write directly to stdout */
    char read_buf[4096];
    int found_output = 0;
    for (;;) {
        ssize_t n = read(pipefd_out[0], read_buf, sizeof(read_buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        fwrite(read_buf, 1, (size_t)n, stdout);
        found_output = 1;
    }
    close(pipefd_out[0]);

    /* Reap child */
    int status;
    pid_t wpid;
    do {
        wpid = waitpid(pid, &status, 0);
    } while (wpid < 0 && errno == EINTR);

    int grep_rc = -1;
    if (wpid > 0 && WIFEXITED(status))
        grep_rc = WEXITSTATUS(status);

    if (grep_rc == 1 && !found_output) {
        fprintf(stderr, "No matches found for pattern: %s\n", pattern);
        return EXIT_ERROR;
    }

    return (grep_rc == 0) ? EXIT_SUCCESS_CODE : EXIT_ERROR;
}

/* --- results --- */

int cmd_results(const char *name, const char *cwd)
{
    if (!name || name[0] == '\0') {
        fprintf(stderr, "Error: results requires <name>\n");
        return EXIT_BAD_ARGS;
    }

    char task_file[PATH_BUF_SIZE];
    build_task_file_path(task_file, sizeof(task_file), cwd, name);

    if (!file_exists(task_file)) {
        fprintf(stderr, "Error: Task file not found for worker '%s'\n", name);
        return EXIT_NOT_FOUND;
    }

    FILE *f = fopen(task_file, "r");
    if (!f) {
        fprintf(stderr, "Error: failed to open task file: %s\n", task_file);
        return EXIT_ERROR;
    }

    char line[LINE_BUF_SIZE];
    int in_log = 0;
    int found_log = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strcmp(line, "## Log\n") == 0 || strcmp(line, "## Log\r\n") == 0 ||
            strcmp(line, "## Log") == 0) {
            in_log = 1;
            found_log = 1;
            printf("%s", line);
            continue;
        }

        if (in_log) {
            /* Stop at next ## heading (but not ### or deeper) */
            if (strncmp(line, "## ", 3) == 0 && strncmp(line, "### ", 4) != 0) {
                break;
            }
            printf("%s", line);
        }
    }

    fclose(f);

    if (!found_log) {
        fprintf(stderr,
                "No Log section found in task file for worker '%s'\n", name);
        return EXIT_ERROR;
    }

    return EXIT_SUCCESS_CODE;
}

/* --- dismiss --- */

int cmd_dismiss(const char *name, const char *cwd)
{
    if (!name || name[0] == '\0') {
        fprintf(stderr, "Error: dismiss requires <name>\n");
        return EXIT_BAD_ARGS;
    }

    char task_file[PATH_BUF_SIZE];
    build_task_file_path(task_file, sizeof(task_file), cwd, name);

    if (!file_exists(task_file)) {
        fprintf(stderr, "Error: Worker '%s' not found\n", name);
        return EXIT_NOT_FOUND;
    }

    char session[NAME_MAX_LEN];
    build_session_name(session, sizeof(session), name);

    /* Kill tmux session if alive */
    if (tmux_has_session(session)) {
        if (tmux_kill_session(session) == 0) {
            printf("Killed tmux session: %s\n", session);
        } else {
            fprintf(stderr,
                    "Warning: tmux kill-session failed for %s\n", session);
        }
    } else {
        printf("tmux session already dead\n");
    }

    /* Update State field to dismissed */
    update_field_in_file(task_file, "State:", "dismissed");

    /* Fill Completed timestamp if empty */
    {
        char timestamp[TIMESTAMP_SIZE];
        get_timestamp(timestamp, sizeof(timestamp));

        size_t file_len = 0;
        char *content = read_file(task_file, &file_len);
        if (content) {
            char *comp_line = strstr(content, "Completed:");
            if (comp_line) {
                const char *val = comp_line + 10; /* strlen("Completed:") */
                while (*val == ' ' || *val == '\t')
                    val++;
                if (*val == '\n' || *val == '\r' || *val == '\0') {
                    update_field_in_file(task_file, "Completed:", timestamp);
                }
            }
            free(content);
        }
    }

    /* Report log file status */
    {
        char log_file[PATH_BUF_SIZE];
        build_log_file_path(log_file, sizeof(log_file), cwd, name);
        if (file_exists(log_file)) {
            printf("Log preserved: %s\n", log_file);
        } else {
            printf("Note: No log file found (worker may not have produced output)\n");
        }
    }

    /* Publish bus event */
    bus_publish(cwd, name, "worker-dismissed", "normal", NULL);

    /* Clean session metadata */
    {
        char session_meta[PATH_BUF_SIZE];
        build_session_file_path(session_meta, sizeof(session_meta), cwd, name);
        unlink(session_meta);
    }

    printf("Dismissed: %s\n", name);
    return EXIT_SUCCESS_CODE;
}

/* --- continue --- */

int cmd_continue(const char *handle, const char *model_override,
                 const char *cwd)
{
    if (!handle || handle[0] == '\0') {
        fprintf(stderr,
                "Error: continue requires <handle>\n"
                "Usage: nbs-workers continue <handle> [--model=MODEL]\n");
        return EXIT_BAD_ARGS;
    }

    char meta_file[PATH_BUF_SIZE];
    build_session_file_path(meta_file, sizeof(meta_file), cwd, handle);

    if (!file_exists(meta_file)) {
        fprintf(stderr,
                "Error: No session metadata for handle '%s'\n"
                "  Expected: %s\n"
                "  Cannot continue without session ID. "
                "Use 'nbs-workers spawn' for a fresh start.\n",
                handle, meta_file);
        return EXIT_NOT_FOUND;
    }

    /* Read session metadata */
    size_t json_len = 0;
    char *json = read_file(meta_file, &json_len);
    if (!json) {
        fprintf(stderr, "Error: failed to read session metadata: %s\n",
                meta_file);
        return EXIT_ERROR;
    }

    char session_id[128] = {0};
    char model[128] = {0};
    char project_root[PATH_BUF_SIZE] = {0};
    char tmux_session_name[NAME_MAX_LEN] = {0};

    json_extract_string(json, "session_id", session_id, sizeof(session_id));
    json_extract_string(json, "model", model, sizeof(model));
    json_extract_string(json, "project_root", project_root,
                        sizeof(project_root));
    json_extract_string(json, "tmux_session", tmux_session_name,
                        sizeof(tmux_session_name));
    long old_pid = json_extract_number(json, "pid");

    free(json);

    /* Validate session ID is UUID */
    if (!validate_uuid(session_id)) {
        fprintf(stderr,
                "Error: Invalid session ID in metadata: '%s'\n"
                "  Cannot resume with a non-UUID session ID.\n",
                session_id);
        return EXIT_ERROR;
    }

    /* Apply model override */
    if (model_override && model_override[0] != '\0') {
        snprintf(model, sizeof(model), "%s", model_override);
    }

    printf("Continuing agent '%s':\n", handle);
    printf("  Session ID: %s\n", session_id);
    printf("  Model: %s\n", model[0] ? model : "<default>");
    printf("  Project: %s\n", project_root);
    printf("  Tmux session: %s\n", tmux_session_name);

    /* Kill old tmux session if still running */
    if (tmux_has_session(tmux_session_name)) {
        printf("  Killing old tmux session...\n");
        tmux_kill_session(tmux_session_name);
        sleep(1);
    }

    /* Clean stale pidfile */
    {
        char pidfile[PATH_BUF_SIZE];
        snprintf(pidfile, sizeof(pidfile), "%s/%s/%s.pid",
                 project_root, PIDS_SUBDIR, handle);
        if (file_exists(pidfile)) {
            if (old_pid > 0 && kill((pid_t)old_pid, 0) != 0) {
                unlink(pidfile);
            }
        }
    }

    /* Build nbs-claude command */
    char nbs_claude_cmd[PATH_BUF_SIZE * 2];
    if (model[0] != '\0') {
        snprintf(nbs_claude_cmd, sizeof(nbs_claude_cmd),
                 "NBS_HANDLE=%s NBS_MODEL=%s bin/nbs-claude "
                 "--continue=%s --dangerously-skip-permissions",
                 handle, model, session_id);
    } else {
        snprintf(nbs_claude_cmd, sizeof(nbs_claude_cmd),
                 "NBS_HANDLE=%s bin/nbs-claude "
                 "--continue=%s --dangerously-skip-permissions",
                 handle, session_id);
    }

    printf("  Spawning: %s\n", nbs_claude_cmd);

    /* Respawn in tmux */
    {
        const char *argv[] = {"tmux", "new-session", "-d",
                              "-s", tmux_session_name,
                              "-c", project_root,
                              nbs_claude_cmd, NULL};
        if (exec_spawn_detached(argv) != 0) {
            fprintf(stderr, "  Warning: tmux session did not start\n");
            return EXIT_ERROR;
        }
    }

    /* Wait and verify */
    sleep(3);
    if (tmux_has_session(tmux_session_name)) {
        printf("  Continued successfully in tmux session: %s\n",
               tmux_session_name);
    } else {
        fprintf(stderr, "  Warning: tmux session did not start\n");
        return EXIT_ERROR;
    }

    /* Publish bus event */
    {
        char payload[256];
        snprintf(payload, sizeof(payload), "Continued session %s",
                 session_id);
        bus_publish(cwd, handle, "worker-continued", "normal", payload);
    }

    return EXIT_SUCCESS_CODE;
}

/* --- session --- */

int cmd_session(const char *handle, const char *cwd)
{
    if (!handle || handle[0] == '\0') {
        fprintf(stderr,
                "Error: session requires <handle>\n"
                "Usage: nbs-workers session <handle>\n");
        return EXIT_BAD_ARGS;
    }

    char meta_file[PATH_BUF_SIZE];
    build_session_file_path(meta_file, sizeof(meta_file), cwd, handle);

    if (!file_exists(meta_file)) {
        fprintf(stderr,
                "No session metadata for handle '%s'\n"
                "  Expected: %s\n"
                "  The agent may not have been started with nbs-claude, "
                "or has exited.\n",
                handle, meta_file);
        return EXIT_NOT_FOUND;
    }

    printf("Session metadata for '%s':\n", handle);
    printf("  File: %s\n", meta_file);

    /* Read and parse JSON */
    size_t json_len = 0;
    char *json = read_file(meta_file, &json_len);
    if (!json) {
        fprintf(stderr, "Error: failed to read session file\n");
        return EXIT_ERROR;
    }

    char session_id[128] = {0};
    char model_buf[128] = {0};
    char started[64] = {0};
    char project_root[PATH_BUF_SIZE] = {0};
    char tmux_session_name[NAME_MAX_LEN] = {0};

    json_extract_string(json, "session_id", session_id, sizeof(session_id));
    json_extract_string(json, "model", model_buf, sizeof(model_buf));
    json_extract_string(json, "started", started, sizeof(started));
    json_extract_string(json, "project_root", project_root,
                        sizeof(project_root));
    json_extract_string(json, "tmux_session", tmux_session_name,
                        sizeof(tmux_session_name));
    long pid_val = json_extract_number(json, "pid");

    free(json);

    printf("  Session ID: %s\n", session_id);
    printf("  Model: %s\n", model_buf[0] ? model_buf : "<default>");
    printf("  Started: %s\n", started);
    printf("  Tmux: %s\n", tmux_session_name);
    printf("  Project: %s\n", project_root);
    printf("  PID: %ld\n", pid_val);

    /* Check if PID is alive */
    if (pid_val > 0 && kill((pid_t)pid_val, 0) == 0) {
        printf("  Status: ALIVE\n");
    } else {
        printf("  Status: DEAD (PID %ld not running)\n", pid_val);
    }

    /* Check tmux session */
    if (tmux_has_session(tmux_session_name)) {
        printf("  Tmux session: ALIVE\n");
    } else {
        printf("  Tmux session: NOT FOUND\n");
    }

    return EXIT_SUCCESS_CODE;
}

/* --- list --- */

int cmd_list(const char *cwd)
{
    printf("NBS Workers:\n");

    char workers_dir[PATH_BUF_SIZE];
    build_workers_dir(workers_dir, sizeof(workers_dir), cwd);

    if (!dir_exists(workers_dir)) {
        printf("  (no workers directory)\n");
        return EXIT_SUCCESS_CODE;
    }

    DIR *d = opendir(workers_dir);
    if (!d) {
        printf("  (no workers directory)\n");
        return EXIT_SUCCESS_CODE;
    }

    int has_workers = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        /* Only look at .md files */
        size_t nlen = strlen(ent->d_name);
        if (nlen < 4 || strcmp(ent->d_name + nlen - 3, ".md") != 0)
            continue;

        /* Extract name (filename without .md extension) */
        char filename[NAME_MAX_LEN];
        size_t base_len = nlen - 3;
        if (base_len >= sizeof(filename))
            continue;
        memcpy(filename, ent->d_name, base_len);
        filename[base_len] = '\0';

        /* Read state from task file */
        char task_path[PATH_BUF_SIZE + NAME_MAX_LEN];
        snprintf(task_path, sizeof(task_path), "%s/%s",
                 workers_dir, ent->d_name);

        char state[64];
        get_state_field(task_path, state, sizeof(state));

        /* Check tmux session */
        char session[NAME_MAX_LEN];
        build_session_name(session, sizeof(session), filename);
        const char *alive = tmux_has_session(session) ? "alive" : "dead";

        /* Check log file */
        char log_info[64];
        char log_path[PATH_BUF_SIZE + NAME_MAX_LEN + 8];
        snprintf(log_path, sizeof(log_path), "%s/%s.log",
                 workers_dir, filename);

        struct stat log_stat;
        if (stat(log_path, &log_stat) == 0) {
            snprintf(log_info, sizeof(log_info), "log:%ldB",
                     (long)log_stat.st_size);
        } else {
            snprintf(log_info, sizeof(log_info), "no-log");
        }

        printf("  %-25s %-12s tmux:%-5s %s\n",
               filename, state, alive, log_info);
        has_workers = 1;
    }

    closedir(d);

    if (!has_workers) {
        printf("  (none)\n");
    }

    return EXIT_SUCCESS_CODE;
}
