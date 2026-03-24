/*
 * main.c — nbs-ts CLI entry point.
 *
 * Architecture: "create" forks a daemon that holds the PTY master fd
 * and runs the output capture loop. The daemon writes the PTY slave
 * device path to the session directory so that "send" can open it
 * directly for input.
 *
 * Exit codes:
 *   0 = success, 1 = error, 2 = not found, 3 = timeout, 4 = bad args
 */

#include "nbs_ts.h"
#include "session_internal.h"
#include "helper_client.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <poll.h>
#include <sys/inotify.h>
#include <pthread.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#endif

#define MAX_DISPLAY_LEN 256
#define MAX_OPTION_VALUE 100000

/*
 * Path buffer sizes:
 *   NBS_TS_MAX_PATH (4096) — for directory paths returned by API
 *   MAX_FILE_PATH (8192) — for dir + filename concatenation
 *
 * GCC's -Wformat-truncation computes worst-case sizes from declared
 * buffer lengths. Since actual paths are ~50 chars (HOME + session dir
 * + 8-char handle), truncation cannot occur in practice. We disable
 * this specific warning for the known-safe snprintf chains in this file.
 */
#define MAX_FILE_PATH 8192

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

/* ── Utilities ────────────────────────────────────────────────────── */

static void sanitise_for_display(const char *input, char *buf, size_t bufsize)
{
    ASSERT_MSG(input != NULL, "sanitise_for_display: input is NULL");
    ASSERT_MSG(buf != NULL, "sanitise_for_display: buf is NULL");
    ASSERT_MSG(bufsize > 0, "sanitise_for_display: bufsize is 0");

    size_t i, max = bufsize - 1;
    for (i = 0; i < max && input[i] != '\0'; i++)
        buf[i] = (input[i] >= 0x20 && input[i] <= 0x7E) ? input[i] : '?';
    buf[i] = '\0';
}

static int parse_int_option(const char *arg, int default_val)
{
    ASSERT_MSG(arg != NULL, "parse_int_option: arg is NULL");

    const char *eq = strchr(arg, '=');
    if (!eq || eq[1] == '\0') return default_val;

    errno = 0;
    char *endptr;
    long val = strtol(eq + 1, &endptr, 10);

    if (errno == ERANGE || *endptr != '\0' || val <= 0 || val > MAX_OPTION_VALUE) {
        fprintf(stderr, "Error: invalid value in '%s': must be 1..%d\n",
                arg, MAX_OPTION_VALUE);
        return -1;
    }
    return (int)val;
}

static char *read_file_str(const char *path, char *buf, size_t bufsize)
{
    ASSERT_MSG(path != NULL, "read_file_str: path is NULL");
    ASSERT_MSG(buf != NULL, "read_file_str: buf is NULL");
    ASSERT_MSG(bufsize > 1, "read_file_str: bufsize too small: %zu", bufsize);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "nbs-ts: read_file_str: open '%s' failed: %s\n",
                path, strerror(errno));
        return NULL;
    }

    ssize_t n = read(fd, buf, bufsize - 1);
    close(fd);
    if (n <= 0) {
        fprintf(stderr, "nbs-ts: read_file_str: read '%s' failed: %s\n",
                path, strerror(errno));
        return NULL;
    }

    buf[n] = '\0';
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
    return buf;
}

static int write_file_str(const char *path, const char *content)
{
    ASSERT_MSG(path != NULL, "write_file_str: path is NULL");
    ASSERT_MSG(content != NULL, "write_file_str: content is NULL");

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "nbs-ts: write_file_str: open '%s' failed: %s\n",
                path, strerror(errno));
        return -1;
    }
    size_t len = strlen(content);
    ssize_t w = write(fd, content, len);
    fsync(fd);
    close(fd);
    if (w != (ssize_t)len) {
        fprintf(stderr, "nbs-ts: write_file_str: write '%s' failed: %s\n",
                path, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * is_valid_handle — Check that handle is exactly 8 hex characters.
 * Prevents path traversal and injection via crafted handles.
 */
static int is_valid_handle(const char *handle)
{
    if (!handle) return 0;
    size_t len = strlen(handle);
    if (len != 8) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = handle[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static int session_exists(const char *handle)
{
    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0) return 0;

    char path[MAX_FILE_PATH];
    snprintf(path, sizeof(path), "%s/pid", dir);

    struct stat st;
    return stat(path, &st) == 0;
}

static pid_t session_pid(const char *handle)
{
    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0) return -1;

    char path[MAX_FILE_PATH];
    snprintf(path, sizeof(path), "%s/pid", dir);

    char buf[32];
    if (!read_file_str(path, buf, sizeof(buf))) return -1;
    errno = 0;
    char *endptr;
    long val = strtol(buf, &endptr, 10);
    if (errno != 0 || endptr == buf || *endptr != '\0' || val <= 0 || val > (long)INT_MAX)
        return -1;
    return (pid_t)val;
}

static int session_is_alive(const char *handle)
{
    pid_t pid = session_pid(handle);
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0;
}

/* ── Daemon relay loop (runs in forked child) ─────────────────────── */

/*
 * daemon_relay — Bidirectional relay between PTY master and files.
 *
 * - Reads PTY master fd → appends to output.log (with fsync)
 * - Reads input.fifo → writes to PTY master fd
 *
 * Uses poll(2) to multiplex both directions. Runs until the master
 * fd returns EOF (child process exited).
 */
static void daemon_relay(int master_fd, const char *output_log_path,
                         const char *input_fifo_path,
                         pid_t child_pid, const char *exit_code_path)
{
    int log_fd = open(output_log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log_fd < 0) {
        fprintf(stderr, "nbs-ts: daemon_relay: open log '%s' failed: %s\n",
                output_log_path, strerror(errno));
        _exit(1);
    }

    /* Create the input FIFO */
    unlink(input_fifo_path);
    if (mkfifo(input_fifo_path, 0600) < 0) {
        fprintf(stderr, "nbs-ts: daemon_relay: mkfifo '%s' failed: %s\n",
                input_fifo_path, strerror(errno));
        close(log_fd);
        _exit(1);
    }

    /* Open FIFO in read-write non-blocking mode. O_RDWR keeps the
     * read end open even when the last writer closes, preventing
     * POLLHUP and eliminating the reopen race window. */
    int fifo_fd = open(input_fifo_path, O_RDWR | O_NONBLOCK);
    if (fifo_fd < 0) {
        fprintf(stderr, "nbs-ts: daemon_relay: open fifo '%s' failed: %s\n",
                input_fifo_path, strerror(errno));
        close(log_fd);
        _exit(1);
    }

    /* Make master_fd non-blocking for poll */
    int flags = fcntl(master_fd, F_GETFL);
    if (flags >= 0) {
        if (fcntl(master_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            fprintf(stderr, "nbs-ts: daemon_relay: fcntl F_SETFL failed: %s\n",
                    strerror(errno));
        }
    }

    char buf[4096];
    int running = 1;

    while (running) {
        struct pollfd pfds[2];
        pfds[0].fd = master_fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = fifo_fd;
        pfds[1].events = POLLIN;

        int pr = poll(pfds, 2, 1000); /* 1s timeout for periodic checks */
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Master → output.log */
        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(log_fd, buf + written, (size_t)(n - written));
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        running = 0;
                        break;
                    }
                    written += w;
                }
                fsync(log_fd);
            } else if (n == 0) {
                running = 0; /* Child exited */
            } else if (errno != EAGAIN && errno != EINTR) {
                running = 0;
            }
        }
        if (pfds[0].revents & (POLLHUP | POLLERR)) {
            /* Drain any remaining data */
            ssize_t n;
            while ((n = read(master_fd, buf, sizeof(buf))) > 0) {
                ssize_t dw = 0;
                while (dw < n) {
                    ssize_t w = write(log_fd, buf + dw, (size_t)(n - dw));
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    dw += w;
                }
            }
            fsync(log_fd);
            running = 0;
        }

        /* Input FIFO → master */
        if (pfds[1].revents & POLLIN) {
            ssize_t n = read(fifo_fd, buf, sizeof(buf));
            if (n > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(master_fd, buf + written, (size_t)(n - written));
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN) { usleep(1000); continue; }
                        break;
                    }
                    written += w;
                }
            }
        }
        /* No POLLHUP handling needed — O_RDWR prevents POLLHUP when
         * the last writer closes the FIFO. */
    }

    close(fifo_fd);
    close(log_fd);
    unlink(input_fifo_path);

    /* Capture the child's exit code */
    if (child_pid > 0 && exit_code_path) {
        int code = 0;
        int got_code = 0;
        int status;
        pid_t r = waitpid(child_pid, &status, 0);
        if (r == child_pid) {
            /* Direct child — we can reap it */
            if (WIFEXITED(status))
                code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                code = 128 + WTERMSIG(status);
            else
                code = 1;
            got_code = 1;
        } else {
            /* Not our child (helper-spawned). Read exit code from
             * completion.log — non-interactive commands write their
             * exit code there explicitly. */
            char comp_path[MAX_FILE_PATH];
            /* exit_code_path is "<session_dir>/exit_code" — derive
             * completion.log from the same directory. */
            {
                /* Find last '/' to get session_dir */
                const char *slash = strrchr(exit_code_path, '/');
                if (slash) {
                    size_t dir_len = (size_t)(slash - exit_code_path);
                    snprintf(comp_path, sizeof(comp_path),
                             "%.*s/completion.log", (int)dir_len, exit_code_path);
                    FILE *cfp = fopen(comp_path, "r");
                    if (cfp) {
                        char line[64];
                        char last_line[64] = "";
                        while (fgets(line, sizeof(line), cfp))
                            memcpy(last_line, line, sizeof(last_line));
                        fclose(cfp);
                        if (last_line[0]) {
                            int seq, rc;
                            if (sscanf(last_line, "%d %d", &seq, &rc) == 2) {
                                code = rc;
                                got_code = 1;
                            }
                        }
                    }
                }
            }
            if (!got_code) {
                code = 0;
                got_code = 1;
            }
        }
        if (got_code) {
            char code_str[32];
            snprintf(code_str, sizeof(code_str), "%d", code);
            int efd = open(exit_code_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (efd < 0) {
                fprintf(stderr, "nbs-ts: daemon: open exit_code '%s' failed: %s\n",
                        exit_code_path, strerror(errno));
            } else {
                ssize_t ew = write(efd, code_str, strlen(code_str));
                if (ew < 0) {
                    fprintf(stderr, "nbs-ts: daemon: write exit_code failed: %s\n",
                            strerror(errno));
                }
                close(efd);
            }
        }
    }
}

/* ── CLI Commands ─────────────────────────────────────────────────── */

static int cmd_create(const char *command)
{
    /* Generate handle */
    char handle[NBS_TS_HANDLE_LEN];
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0) { perror("nbs-ts: open urandom"); return NBS_TS_EXIT_ERROR; }
        unsigned char bytes[4];
        ssize_t n = read(fd, bytes, sizeof(bytes));
        close(fd);
        if (n != 4) return NBS_TS_EXIT_ERROR;
        snprintf(handle, sizeof(handle), "%02x%02x%02x%02x",
                 bytes[0], bytes[1], bytes[2], bytes[3]);
        ASSERT_MSG(is_valid_handle(handle), "cmd_create: generated handle '%s' is invalid", handle);
    }

    /* Create session directory */
    char sessions_dir[NBS_TS_MAX_PATH];
    if (nbs_ts_sessions_dir(sessions_dir, sizeof(sessions_dir)) < 0)
        return NBS_TS_EXIT_ERROR;

    char session_dir[MAX_FILE_PATH];
    snprintf(session_dir, sizeof(session_dir), "%s/%s", sessions_dir, handle);
    if (mkdir(session_dir, 0700) < 0) {
        perror("nbs-ts: mkdir session");
        return NBS_TS_EXIT_ERROR;
    }

    /* Set up file paths */
    char output_log[MAX_FILE_PATH], completion_log[MAX_FILE_PATH];
    char pid_path[MAX_FILE_PATH], meta_path[MAX_FILE_PATH];
    char slave_path_file[MAX_FILE_PATH], daemon_pid_path[MAX_FILE_PATH];
    char input_fifo[MAX_FILE_PATH];

    snprintf(output_log, sizeof(output_log), "%s/output.log", session_dir);
    snprintf(completion_log, sizeof(completion_log), "%s/completion.log", session_dir);
    snprintf(pid_path, sizeof(pid_path), "%s/pid", session_dir);
    snprintf(meta_path, sizeof(meta_path), "%s/meta", session_dir);
    snprintf(slave_path_file, sizeof(slave_path_file), "%s/slave_pty", session_dir);
    snprintf(daemon_pid_path, sizeof(daemon_pid_path), "%s/daemon_pid", session_dir);
    snprintf(input_fifo, sizeof(input_fifo), "%s/input.fifo", session_dir);

    /* Create empty log files */
    {
        int ofd = open(output_log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (ofd < 0) {
            fprintf(stderr, "nbs-ts: create output log '%s' failed: %s\n",
                    output_log, strerror(errno));
            rmdir(session_dir);
            return NBS_TS_EXIT_ERROR;
        }
        close(ofd);
        int cfd = open(completion_log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (cfd < 0) {
            fprintf(stderr, "nbs-ts: create completion log '%s' failed: %s\n",
                    completion_log, strerror(errno));
            unlink(output_log);
            rmdir(session_dir);
            return NBS_TS_EXIT_ERROR;
        }
        close(cfd);
    }

    /* Write the rcfile for PROMPT_COMMAND — needed by both helper and direct paths */
    char rcfile_path[MAX_FILE_PATH];
    snprintf(rcfile_path, sizeof(rcfile_path), "%s/bashrc", session_dir);

    char rcfile_content[MAX_FILE_PATH * 2];
    snprintf(rcfile_content, sizeof(rcfile_content),
             "NBS_TS_SEQ=-1\n"
             "PROMPT_COMMAND='NBS_TS_LAST_EXIT=$?; "
             "NBS_TS_SEQ=$((NBS_TS_SEQ + 1)); "
             "echo \"$NBS_TS_SEQ $NBS_TS_LAST_EXIT\" >> \"%s\"'\n",
             completion_log);

    int rcfd = open(rcfile_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (rcfd < 0) {
        fprintf(stderr, "nbs-ts: create rcfile '%s' failed: %s\n",
                rcfile_path, strerror(errno));
        rmdir(session_dir);
        return NBS_TS_EXIT_ERROR;
    }
    {
        size_t rc_len = strlen(rcfile_content);
        ssize_t rc_w = write(rcfd, rcfile_content, rc_len);
        close(rcfd);
        if (rc_w != (ssize_t)rc_len) {
            fprintf(stderr, "nbs-ts: write rcfile '%s' failed: %s\n",
                    rcfile_path, strerror(errno));
            rmdir(session_dir);
            return NBS_TS_EXIT_ERROR;
        }
    }

    /* Create PTY — try helper first, fall back to direct openpty. */
    int master_fd, slave_fd = -1;
    char slave_name[256] = "";
    int used_helper = 0;
    pid_t helper_child_pid = 0;

    {
        char helper_cmd[MAX_FILE_PATH * 4];
        if (strcmp(command, "bash") == 0 || strcmp(command, "bash -i") == 0) {
            snprintf(helper_cmd, sizeof(helper_cmd),
                     "bash --rcfile \"%s\" -i", rcfile_path);
        } else {
            /* Non-interactive: use trap EXIT to capture exit code.
             * PROMPT_COMMAND doesn't fire in non-interactive bash, and
             * `exit N` terminates before any trailing commands run, so
             * a trap is the only reliable mechanism.  The daemon can't
             * waitpid on helper-spawned children. */
            snprintf(helper_cmd, sizeof(helper_cmd),
                     "trap 'echo \"0 $?\" >> \"%s\"' EXIT; "
                     "source \"%s\"; %s",
                     completion_log, rcfile_path, command);
        }

        int hfd = helper_request_pty(helper_cmd, &helper_child_pid);
        if (hfd >= 0) {
            master_fd = hfd;
            used_helper = 1;
        }
    }

    if (!used_helper) {
        static int helper_warned = 0;
        if (!helper_warned) {
            fprintf(stderr, "nbs-ts: helper not running — using direct fork "
                    "(start nbs-ts-helper for full capabilities)\n");
            helper_warned = 1;
        }

        struct winsize ws = { .ws_row = 24, .ws_col = 80 };
        if (openpty(&master_fd, &slave_fd, slave_name, NULL, &ws) < 0) {
            perror("nbs-ts: openpty");
            rmdir(session_dir);
            return NBS_TS_EXIT_ERROR;
        }
    }

    /* Save slave PTY path */
    if (write_file_str(slave_path_file, slave_name) < 0) {
        fprintf(stderr, "nbs-ts: write slave path failed\n");
        close(master_fd);
        if (slave_fd >= 0) close(slave_fd);
        return NBS_TS_EXIT_ERROR;
    }

    /* Write metadata */
    char meta_buf[MAX_FILE_PATH * 2];
    snprintf(meta_buf, sizeof(meta_buf), "command: %s\nstart: %ld\n",
             command, (long)time(NULL));
    if (write_file_str(meta_path, meta_buf) < 0)
        fprintf(stderr, "nbs-ts: warning: failed to write metadata\n");

    /*
     * Fork the daemon. The daemon then forks the child.
     * This ensures the daemon is the child's parent and can waitpid.
     *
     * Process tree:
     *   nbs-ts create (exits immediately)
     *     └── daemon (persists, captures output)
     *           └── child (runs the user's command)
     */
    pid_t daemon_pid = fork();
    if (daemon_pid < 0) {
        perror("nbs-ts: fork daemon");
        close(master_fd);
        close(slave_fd);
        return NBS_TS_EXIT_ERROR;
    }

    if (daemon_pid == 0) {
        /* ── Daemon process ── */
        setsid();

        pid_t child_pid = 0;

        if (used_helper) {
            /* Helper forked the child and sent us its PID. */
            child_pid = helper_child_pid;
        } else {
            /* Direct mode: fork the child from within the daemon */
            child_pid = fork();
            if (child_pid < 0) _exit(1);

            if (child_pid == 0) {
                /* ── Child: run the command in the PTY ── */
                close(master_fd);
                setsid();
                ioctl(slave_fd, TIOCSCTTY, 0);

                dup2(slave_fd, STDIN_FILENO);
                dup2(slave_fd, STDOUT_FILENO);
                dup2(slave_fd, STDERR_FILENO);
                if (slave_fd > STDERR_FILENO) close(slave_fd);

                unsetenv("TMUX");
                setenv("BASH_ENV", rcfile_path, 1);
                setenv("NBS_TS_COMPLETION_LOG", completion_log, 1);

                char pc[MAX_FILE_PATH * 2];
                snprintf(pc, sizeof(pc),
                         "NBS_TS_LAST_EXIT=$?; "
                         "NBS_TS_SEQ=$((${NBS_TS_SEQ:--1} + 1)); "
                         "echo \"$NBS_TS_SEQ $NBS_TS_LAST_EXIT\" >> \"%s\"",
                         completion_log);
                setenv("PROMPT_COMMAND", pc, 1);
                setenv("NBS_TS_SEQ", "-1", 1);

                if (strcmp(command, "bash") == 0 || strcmp(command, "bash -i") == 0) {
                    execlp("bash", "bash", "--rcfile", rcfile_path, "-i",
                           (char *)NULL);
                } else {
                    char setup[MAX_FILE_PATH * 2];
                    snprintf(setup, sizeof(setup),
                             "source \"%s\"; %s",
                             rcfile_path, command);
                    execlp("bash", "bash", "-c", setup, (char *)NULL);
                }
                _exit(127);
            }
        }

        /* Daemon: child is running. Write its PID. */
        if (slave_fd >= 0) close(slave_fd);

        char buf[64];
        snprintf(buf, sizeof(buf), "%d", (int)child_pid);
        write_file_str(pid_path, buf);

        /* Redirect daemon stdio to /dev/null */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }

        char exit_code_file[MAX_FILE_PATH];
        snprintf(exit_code_file, sizeof(exit_code_file),
                 "%s/exit_code", session_dir);

        /* Run the relay loop (captures output + forwards input) */
        daemon_relay(master_fd, output_log, input_fifo,
                     child_pid, exit_code_file);
        close(master_fd);
        _exit(0);
    }

    /* ── Original process: write daemon PID and exit ── */
    close(master_fd);
    close(slave_fd);

    char buf[64];
    snprintf(buf, sizeof(buf), "%d", (int)daemon_pid);
    write_file_str(daemon_pid_path, buf);

    /* Wait briefly for daemon to write child PID */
    usleep(100000);

    printf("%s\n", handle);
    return NBS_TS_EXIT_SUCCESS;
}

static int cmd_send(const char *handle, const char *text, size_t len)
{
    if (!session_exists(handle)) {
        fprintf(stderr, "nbs-ts: session '%s' not found\n", handle);
        return NBS_TS_EXIT_NOT_FOUND;
    }

    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0)
        return NBS_TS_EXIT_ERROR;

    /*
     * Write to the input FIFO. The daemon reads this and writes to
     * the PTY master fd. This is the only safe way to inject input
     * without TIOCSTI (disabled in Linux 6.2+).
     */
    char fifo_path[MAX_FILE_PATH];
    snprintf(fifo_path, sizeof(fifo_path), "%s/input.fifo", dir);

    /* Open FIFO for writing (blocks until daemon has it open for reading) */
    int fd = open(fifo_path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "nbs-ts: cannot open input FIFO for session '%s'\n", handle);
        return NBS_TS_EXIT_ERROR;
    }

    /* No paste bracket wrapping in the CLI — bash and other non-TUI
     * consumers don't need it. Paste brackets are applied only in the
     * sidecar transport (transport_ts.c ts_send_text) which exclusively
     * sends to Claude Code's TUI. */
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, text + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            fprintf(stderr, "nbs-ts: write failed\n");
            return NBS_TS_EXIT_ERROR;
        }
        written += (size_t)w;
    }

    close(fd);
    return NBS_TS_EXIT_SUCCESS;
}

static int cmd_read_new(const char *handle, int strip)
{
    if (!session_exists(handle)) {
        fprintf(stderr, "nbs-ts: session '%s' not found\n", handle);
        return NBS_TS_EXIT_NOT_FOUND;
    }

    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0)
        return NBS_TS_EXIT_ERROR;

    char log_path[MAX_FILE_PATH];
    snprintf(log_path, sizeof(log_path), "%s/output.log", dir);

    char cursor_path[MAX_FILE_PATH];
    snprintf(cursor_path, sizeof(cursor_path), "%s/read_cursor", dir);

    off_t cursor = 0;
    char cursor_buf[32];
    if (read_file_str(cursor_path, cursor_buf, sizeof(cursor_buf))) {
        errno = 0;
        char *endptr;
        long val = strtol(cursor_buf, &endptr, 10);
        if (errno == 0 && endptr != cursor_buf && *endptr == '\0' && val >= 0)
            cursor = (off_t)val;
    }

    int fd = open(log_path, O_RDONLY);
    if (fd < 0) return NBS_TS_EXIT_SUCCESS;

    char buf[65536];
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, cursor);
    close(fd);

    if (n <= 0) return NBS_TS_EXIT_SUCCESS;
    buf[n] = '\0';

    /* Update cursor */
    char new_cursor[32];
    snprintf(new_cursor, sizeof(new_cursor), "%ld", (long)(cursor + n));
    write_file_str(cursor_path, new_cursor);

    if (strip) {
        char *rd = buf, *wr = buf;
        while (*rd) {
            if (*rd == '\x1b') {
                rd++;
                if (*rd == '[') {
                    rd++;
                    while (*rd && ((unsigned char)*rd < 0x40 || (unsigned char)*rd > 0x7E)) rd++;
                    if (*rd) rd++;
                } else if (*rd == ']') {
                    rd++;
                    while (*rd) {
                        if (*rd == '\x07') { rd++; break; }
                        if (*rd == '\x1b' && *(rd+1) == '\\') { rd += 2; break; }
                        rd++;
                    }
                } else if ((unsigned char)*rd >= 0x20 && (unsigned char)*rd <= 0x7E) {
                    rd++;
                }
            } else if (((unsigned char)*rd < 0x20 && *rd != '\n' && *rd != '\t') ||
                       (unsigned char)*rd == 0x7F) {
                rd++;
            } else {
                *wr++ = *rd++;
            }
        }
        *wr = '\0';
    }

    fputs(buf, stdout);
    return NBS_TS_EXIT_SUCCESS;
}

static int cmd_read(const char *handle, off_t offset)
{
    if (!session_exists(handle)) {
        fprintf(stderr, "nbs-ts: session '%s' not found\n", handle);
        return NBS_TS_EXIT_NOT_FOUND;
    }

    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0)
        return NBS_TS_EXIT_ERROR;

    char log_path[MAX_FILE_PATH];
    snprintf(log_path, sizeof(log_path), "%s/output.log", dir);

    int fd = open(log_path, O_RDONLY);
    if (fd < 0) return NBS_TS_EXIT_ERROR;

    char buf[65536];
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, offset);
    close(fd);

    if (n <= 0) return NBS_TS_EXIT_SUCCESS;
    buf[n] = '\0';
    fputs(buf, stdout);
    return NBS_TS_EXIT_SUCCESS;
}

static int cmd_read_tail(const char *handle, int n_lines)
{
    if (!session_exists(handle)) {
        fprintf(stderr, "nbs-ts: session '%s' not found\n", handle);
        return NBS_TS_EXIT_NOT_FOUND;
    }

    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0)
        return NBS_TS_EXIT_ERROR;

    char log_path[MAX_FILE_PATH];
    snprintf(log_path, sizeof(log_path), "%s/output.log", dir);

    int fd = open(log_path, O_RDONLY);
    if (fd < 0) return NBS_TS_EXIT_ERROR;

    /* Read the tail portion */
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { close(fd); return NBS_TS_EXIT_SUCCESS; }

    size_t read_size = (size_t)file_size;
    if (read_size > 65535) read_size = 65535;

    off_t read_offset = file_size - (off_t)read_size;
    char buf[65536];
    ssize_t n = pread(fd, buf, read_size, read_offset);
    close(fd);

    if (n <= 0) return NBS_TS_EXIT_SUCCESS;

    /* Scan backwards to find start of last n_lines lines */
    int lines_found = 0;
    ssize_t pos = n - 1;
    if (pos >= 0 && buf[pos] == '\n') pos--;
    while (pos >= 0 && lines_found < n_lines) {
        if (buf[pos] == '\n') lines_found++;
        if (lines_found < n_lines) pos--;
    }
    ssize_t start = (pos < 0) ? 0 : pos + 1;

    buf[n] = '\0';
    fputs(buf + start, stdout);
    return NBS_TS_EXIT_SUCCESS;
}

static int cmd_status(const char *handle)
{
    if (!session_exists(handle)) {
        fprintf(stderr, "nbs-ts: session '%s' not found\n", handle);
        return NBS_TS_EXIT_NOT_FOUND;
    }

    int alive = session_is_alive(handle);
    printf("%s\n", alive ? "alive" : "dead");

    if (!alive) {
        /* Read exit code from file (written by daemon on child exit).
         * The child's parent is the daemon, not the CLI — waitpid
         * would return ECHILD here. */
        char dir[NBS_TS_MAX_PATH];
        if (nbs_ts_session_dir(handle, dir, sizeof(dir)) == 0) {
            char ec_path[MAX_FILE_PATH];
            snprintf(ec_path, sizeof(ec_path), "%s/exit_code", dir);
            char ec_buf[32];
            if (read_file_str(ec_path, ec_buf, sizeof(ec_buf)))
                printf("exit_code: %s\n", ec_buf);
        }
    }
    return NBS_TS_EXIT_SUCCESS;
}

static int cmd_exit_code(const char *handle)
{
    if (!session_exists(handle)) {
        fprintf(stderr, "nbs-ts: session '%s' not found\n", handle);
        return NBS_TS_EXIT_NOT_FOUND;
    }

    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0)
        return NBS_TS_EXIT_ERROR;

    /* Read exit code from file (written by daemon on child exit) */
    char ec_path[MAX_FILE_PATH];
    snprintf(ec_path, sizeof(ec_path), "%s/exit_code", dir);

    char ec_buf[32];
    if (read_file_str(ec_path, ec_buf, sizeof(ec_buf))) {
        printf("%s\n", ec_buf);
    } else {
        /* Not yet exited or file not written */
        printf("-1\n");
    }

    return NBS_TS_EXIT_SUCCESS;
}

static int cmd_wait_pattern(const char *handle, const char *pattern,
                            int timeout_sec)
{
    if (!session_exists(handle)) {
        fprintf(stderr, "nbs-ts: session '%s' not found\n", handle);
        return NBS_TS_EXIT_NOT_FOUND;
    }

    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0)
        return NBS_TS_EXIT_ERROR;

    char log_path[MAX_FILE_PATH];
    snprintf(log_path, sizeof(log_path), "%s/output.log", dir);

    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) return NBS_TS_EXIT_ERROR;

    int wd = inotify_add_watch(ifd, log_path, IN_MODIFY | IN_CLOSE_WRITE);
    if (wd < 0) { close(ifd); return NBS_TS_EXIT_ERROR; }

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        fprintf(stderr, "nbs-ts: cmd_wait_pattern: clock_gettime failed: %s\n",
                strerror(errno));
        inotify_rm_watch(ifd, wd);
        close(ifd);
        return NBS_TS_EXIT_ERROR;
    }
    deadline.tv_sec += timeout_sec;

    int found = 0;
    size_t search_offset = 0;

    for (;;) {
        int fd = open(log_path, O_RDONLY);
        if (fd >= 0) {
            off_t end = lseek(fd, 0, SEEK_END);
            if (end > (off_t)search_offset) {
                size_t to_read = (size_t)(end - (off_t)search_offset);
                if (to_read > 1024 * 1024) to_read = 1024 * 1024;
                char *buf = malloc(to_read + 1);
                if (!buf) {
                    fprintf(stderr, "nbs-ts: cmd_wait_pattern: malloc(%zu) failed: %s\n",
                            to_read + 1, strerror(errno));
                    close(fd);
                    inotify_rm_watch(ifd, wd);
                    close(ifd);
                    return NBS_TS_EXIT_ERROR;
                }
                {
                    ssize_t n = pread(fd, buf, to_read, (off_t)search_offset);
                    if (n > 0) {
                        buf[n] = '\0';
                        if (strstr(buf, pattern)) found = 1;
                        search_offset += (size_t)n;
                    }
                    free(buf);
                }
            }
            close(fd);
        }

        if (found) break;

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) break;
        long long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000LL +
                                 (deadline.tv_nsec - now.tv_nsec) / 1000000LL;
        if (remaining_ms <= 0) break;

        struct pollfd pfd = { .fd = ifd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)(remaining_ms > INT_MAX ? INT_MAX : remaining_ms));
        if (pr > 0) {
            char evbuf[256];
            while (read(ifd, evbuf, sizeof(evbuf)) > 0) ;
        } else if (pr == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);
    return found ? NBS_TS_EXIT_SUCCESS : NBS_TS_EXIT_TIMEOUT;
}

static int cmd_wait_complete(const char *handle, int timeout_sec)
{
    if (!session_exists(handle)) {
        fprintf(stderr, "nbs-ts: session '%s' not found\n", handle);
        return NBS_TS_EXIT_NOT_FOUND;
    }

    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0)
        return NBS_TS_EXIT_ERROR;

    char comp_path[MAX_FILE_PATH];
    snprintf(comp_path, sizeof(comp_path), "%s/completion.log", dir);

    char cursor_path[MAX_FILE_PATH];
    snprintf(cursor_path, sizeof(cursor_path), "%s/completion_cursor", dir);

    unsigned long cursor = 0;
    char cursor_buf[32];
    if (read_file_str(cursor_path, cursor_buf, sizeof(cursor_buf)))
        cursor = strtoul(cursor_buf, NULL, 10);

    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) return NBS_TS_EXIT_ERROR;

    int wd = inotify_add_watch(ifd, comp_path, IN_MODIFY | IN_CLOSE_WRITE);
    if (wd < 0) { close(ifd); return NBS_TS_EXIT_ERROR; }

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        fprintf(stderr, "nbs-ts: cmd_wait_complete: clock_gettime failed: %s\n",
                strerror(errno));
        inotify_rm_watch(ifd, wd);
        close(ifd);
        return NBS_TS_EXIT_ERROR;
    }
    deadline.tv_sec += timeout_sec;

    int found = 0;
    nbs_ts_completion_t result = {0, 0};

    for (;;) {
        FILE *f = fopen(comp_path, "r");
        if (f) {
            char line[128];
            unsigned long seq;
            int code;
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%lu %d", &seq, &code) == 2 && seq > cursor) {
                    result.seq = seq;
                    result.exit_code = code;
                    cursor = seq;
                    found = 1;
                }
            }
            fclose(f);
        }

        if (found) break;

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) break;
        long long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000LL +
                                 (deadline.tv_nsec - now.tv_nsec) / 1000000LL;
        if (remaining_ms <= 0) break;

        struct pollfd pfd = { .fd = ifd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)(remaining_ms > INT_MAX ? INT_MAX : remaining_ms));
        if (pr > 0) {
            char evbuf[256];
            while (read(ifd, evbuf, sizeof(evbuf)) > 0) ;
        } else if (pr == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);

    if (found) {
        char new_cursor[32];
        snprintf(new_cursor, sizeof(new_cursor), "%lu", cursor);
        write_file_str(cursor_path, new_cursor);
        printf("%d\n", result.exit_code);
        return NBS_TS_EXIT_SUCCESS;
    }
    return NBS_TS_EXIT_TIMEOUT;
}

static int cmd_kill(const char *handle)
{
    if (!session_exists(handle)) {
        fprintf(stderr, "nbs-ts: session '%s' not found\n", handle);
        return NBS_TS_EXIT_NOT_FOUND;
    }

    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0)
        return NBS_TS_EXIT_ERROR;

    /* Kill child process group. The child calls setsid() so it is a
     * process group leader. Negative PID sends signal to the entire
     * group, preventing orphan subprocesses. */
    pid_t pid = session_pid(handle);
    if (pid > 0) {
        kill(-pid, SIGTERM);
        usleep(100000);
        if (kill(pid, 0) == 0) kill(-pid, SIGKILL);
        waitpid(pid, NULL, WNOHANG);
    }

    /* Kill daemon */
    char dpid_path[MAX_FILE_PATH];
    snprintf(dpid_path, sizeof(dpid_path), "%s/daemon_pid", dir);
    char dpid_buf[32];
    if (read_file_str(dpid_path, dpid_buf, sizeof(dpid_buf))) {
        errno = 0;
        char *endptr;
        long dpid_val = strtol(dpid_buf, &endptr, 10);
        pid_t dpid = (dpid_val > 1 && dpid_val <= (long)INT_MAX &&
                      errno == 0 && endptr != dpid_buf && *endptr == '\0')
                     ? (pid_t)dpid_val : -1;
        if (dpid > 1) {
            kill(dpid, SIGTERM);
            usleep(50000);
            if (kill(dpid, 0) == 0) kill(dpid, SIGKILL);
        }
    }

    /* Clean up files */
    char path[MAX_FILE_PATH];
    const char *files[] = {
        "output.log", "completion.log", "pid", "meta",
        "daemon_pid", "slave_pty", "read_cursor", "completion_cursor",
        "input.fifo", "bashrc", "exit_code", NULL
    };
    for (int i = 0; files[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        unlink(path);
    }
    rmdir(dir);

    return NBS_TS_EXIT_SUCCESS;
}

static int cmd_list(void)
{
    char sessions_dir[NBS_TS_MAX_PATH];
    if (nbs_ts_sessions_dir(sessions_dir, sizeof(sessions_dir)) < 0)
        return NBS_TS_EXIT_SUCCESS;

    DIR *d = opendir(sessions_dir);
    if (!d) return NBS_TS_EXIT_SUCCESS;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!is_valid_handle(ent->d_name)) continue;

        char meta_path[MAX_FILE_PATH];
        snprintf(meta_path, sizeof(meta_path), "%s/%s/meta",
                 sessions_dir, ent->d_name);

        struct stat st;
        if (stat(meta_path, &st) != 0) continue;

        int alive = session_is_alive(ent->d_name);

        char meta_buf[1024];
        char *meta = read_file_str(meta_path, meta_buf, sizeof(meta_buf));

        const char *cmd_str = "unknown";
        if (meta) {
            char *cmd_line = strstr(meta, "command: ");
            if (cmd_line) {
                cmd_str = cmd_line + 9;
                char *nl = strchr(cmd_str, '\n');
                if (nl) *nl = '\0';
            }
        }

        printf("%s\t%s\t%s\n", ent->d_name, alive ? "alive" : "dead", cmd_str);
    }

    closedir(d);
    return NBS_TS_EXIT_SUCCESS;
}

static int cmd_attach(const char *handle)
{
    if (!session_exists(handle)) {
        fprintf(stderr, "nbs-ts: session '%s' not found\n", handle);
        return NBS_TS_EXIT_NOT_FOUND;
    }

    char dir[NBS_TS_MAX_PATH];
    if (nbs_ts_session_dir(handle, dir, sizeof(dir)) < 0)
        return NBS_TS_EXIT_ERROR;

    char log_path[MAX_FILE_PATH];
    snprintf(log_path, sizeof(log_path), "%s/output.log", dir);

    execlp("tail", "tail", "-f", log_path, (char *)NULL);
    fprintf(stderr, "nbs-ts: exec tail failed\n");
    return NBS_TS_EXIT_ERROR;
}

static int cmd_help(void)
{
    printf("Usage: nbs-ts <command> [options]\n\n");
    printf("Commands:\n");
    printf("  create <command>                  Create a new session\n");
    printf("  send <handle> <text>              Send text to a session\n");
    printf("  read-new <handle> [--strip]       Read new output since last read\n");
    printf("  read <handle> [--offset=N|--last=N] Read output from offset or last N lines\n");
    printf("  wait-complete <handle> [--timeout=N]  Wait for command completion\n");
    printf("                                        (does not detect exit/logout — use exit-code)\n");
    printf("  wait-pattern <handle> <pattern> [--timeout=N]  Wait for pattern\n");
    printf("  status <handle>                   Check session status\n");
    printf("  exit-code <handle>                Get exit code\n");
    printf("  kill <handle>                     Terminate session\n");
    printf("  list                              List all sessions\n");
    printf("  attach <handle>                   Tail output (human viewer)\n");
    printf("  help                              Show this help\n\n");
    printf("Exit codes: 0=success 1=error 2=not-found 3=timeout 4=bad-args\n");
    return NBS_TS_EXIT_SUCCESS;
}

/* ── Dispatch ─────────────────────────────────────────────────────── */

static char *join_args(int argc, char *argv[], int start)
{
    size_t total = 0;
    for (int i = start; i < argc; i++) {
        total += strlen(argv[i]);
        if (i < argc - 1) total++;
    }
    char *buf = malloc(total + 1);
    if (!buf) {
        fprintf(stderr, "nbs-ts: join_args: malloc(%zu) failed: %s\n",
                total + 1, strerror(errno));
        return NULL;
    }
    size_t pos = 0;
    for (int i = start; i < argc; i++) {
        size_t len = strlen(argv[i]);
        memcpy(buf + pos, argv[i], len);
        pos += len;
        if (i < argc - 1) buf[pos++] = ' ';
    }
    buf[pos] = '\0';
    return buf;
}

static int require_valid_handle(const char *handle)
{
    if (!is_valid_handle(handle)) {
        fprintf(stderr, "Error: invalid handle '%s': must be 8 hex characters\n",
                handle);
        return NBS_TS_EXIT_BAD_ARGS;
    }
    return 0;
}

#ifndef TEST_BUILD
int main(int argc, char *argv[])
{
    ASSERT_MSG(argv != NULL, "main: argv is NULL");

    if (argc < 2) return cmd_help();

    const char *cmd = argv[1];

    if (strcmp(cmd, "create") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: create requires <command>\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        char *command = join_args(argc, argv, 2);
        if (!command) return NBS_TS_EXIT_ERROR;
        int rc = cmd_create(command);
        free(command);
        return rc;

    } else if (strcmp(cmd, "send") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: send requires <handle> <text>\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        int hrc = require_valid_handle(argv[2]);
        if (hrc) return hrc;
        char *text = join_args(argc, argv, 3);
        if (!text) return NBS_TS_EXIT_ERROR;
        size_t len = strlen(text);
        /* Append carriage return (Enter in raw terminal mode) */
        char *with_nl = realloc(text, len + 2);
        if (!with_nl) { free(text); return NBS_TS_EXIT_ERROR; }
        with_nl[len] = '\r';
        with_nl[len + 1] = '\0';
        int rc = cmd_send(argv[2], with_nl, len + 1);
        free(with_nl);
        return rc;

    } else if (strcmp(cmd, "read-new") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: read-new requires <handle>\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        int hrc = require_valid_handle(argv[2]);
        if (hrc) return hrc;
        int strip = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--strip") == 0) strip = 1;
            else {
                char safe[MAX_DISPLAY_LEN];
                sanitise_for_display(argv[i], safe, sizeof(safe));
                fprintf(stderr, "Error: unrecognised option '%s'\n", safe);
                return NBS_TS_EXIT_BAD_ARGS;
            }
        }
        return cmd_read_new(argv[2], strip);

    } else if (strcmp(cmd, "read") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: read requires <handle>\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        { int hrc = require_valid_handle(argv[2]); if (hrc) return hrc; }
        off_t offset = 0;
        int last_n = 0;
        int have_offset = 0;
        for (int i = 3; i < argc; i++) {
            if (strncmp(argv[i], "--offset=", 9) == 0) {
                int val = parse_int_option(argv[i], 0);
                if (val == -1) return NBS_TS_EXIT_BAD_ARGS;
                offset = val;
                have_offset = 1;
            } else if (strncmp(argv[i], "--last=", 7) == 0) {
                int val = parse_int_option(argv[i], 0);
                if (val == -1) return NBS_TS_EXIT_BAD_ARGS;
                last_n = val;
            } else {
                char safe[MAX_DISPLAY_LEN];
                sanitise_for_display(argv[i], safe, sizeof(safe));
                fprintf(stderr, "Error: unrecognised option '%s'\n", safe);
                return NBS_TS_EXIT_BAD_ARGS;
            }
        }
        if (last_n > 0 && have_offset) {
            fprintf(stderr, "Error: --last and --offset are mutually exclusive\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        if (last_n > 0)
            return cmd_read_tail(argv[2], last_n);
        return cmd_read(argv[2], offset);

    } else if (strcmp(cmd, "wait-complete") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: wait-complete requires <handle>\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        { int hrc = require_valid_handle(argv[2]); if (hrc) return hrc; }
        int timeout = 60;
        for (int i = 3; i < argc; i++) {
            if (strncmp(argv[i], "--timeout=", 10) == 0) {
                timeout = parse_int_option(argv[i], timeout);
                if (timeout == -1) return NBS_TS_EXIT_BAD_ARGS;
            } else {
                char safe[MAX_DISPLAY_LEN];
                sanitise_for_display(argv[i], safe, sizeof(safe));
                fprintf(stderr, "Error: unrecognised option '%s'\n", safe);
                return NBS_TS_EXIT_BAD_ARGS;
            }
        }
        return cmd_wait_complete(argv[2], timeout);

    } else if (strcmp(cmd, "wait-pattern") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: wait-pattern requires <handle> <pattern>\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        { int hrc = require_valid_handle(argv[2]); if (hrc) return hrc; }
        int timeout = 60;
        for (int i = 4; i < argc; i++) {
            if (strncmp(argv[i], "--timeout=", 10) == 0) {
                timeout = parse_int_option(argv[i], timeout);
                if (timeout == -1) return NBS_TS_EXIT_BAD_ARGS;
            } else {
                char safe[MAX_DISPLAY_LEN];
                sanitise_for_display(argv[i], safe, sizeof(safe));
                fprintf(stderr, "Error: unrecognised option '%s'\n", safe);
                return NBS_TS_EXIT_BAD_ARGS;
            }
        }
        return cmd_wait_pattern(argv[2], argv[3], timeout);

    } else if (strcmp(cmd, "status") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: status requires <handle>\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        { int hrc = require_valid_handle(argv[2]); if (hrc) return hrc; }
        return cmd_status(argv[2]);

    } else if (strcmp(cmd, "exit-code") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: exit-code requires <handle>\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        { int hrc = require_valid_handle(argv[2]); if (hrc) return hrc; }
        return cmd_exit_code(argv[2]);

    } else if (strcmp(cmd, "kill") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: kill requires <handle>\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        { int hrc = require_valid_handle(argv[2]); if (hrc) return hrc; }
        return cmd_kill(argv[2]);

    } else if (strcmp(cmd, "list") == 0) {
        return cmd_list();

    } else if (strcmp(cmd, "attach") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: attach requires <handle>\n");
            return NBS_TS_EXIT_BAD_ARGS;
        }
        { int hrc = require_valid_handle(argv[2]); if (hrc) return hrc; }
        return cmd_attach(argv[2]);

    } else if (strcmp(cmd, "help") == 0 ||
               strcmp(cmd, "--help") == 0 ||
               strcmp(cmd, "-h") == 0) {
        return cmd_help();

    } else {
        char safe[MAX_DISPLAY_LEN];
        sanitise_for_display(cmd, safe, sizeof(safe));
        fprintf(stderr, "Unknown command: %s\n", safe);
        fprintf(stderr, "Run 'nbs-ts help' for usage\n");
        return NBS_TS_EXIT_BAD_ARGS;
    }
}
#endif /* TEST_BUILD */
