/*
 * exec_util.c — Generic fork+exec+capture utilities.
 *
 * Generalises the fork+exec pattern from bus_bridge.c (lines 239-322)
 * into two reusable functions: exec_capture and exec_fire_and_forget.
 *
 * Invariants:
 *   - Child always uses _exit(), never exit()
 *   - stderr is always redirected to /dev/null (or closed)
 *   - waitpid retries on EINTR
 *   - out_buf is always NUL-terminated on success
 *   - Pipe fds use O_CLOEXEC to prevent leakage to child
 *
 * Note on exec_fire_and_forget: despite the name suggesting asynchronous
 * semantics, this function synchronously waits for the child to exit.
 * The "forget" refers to discarding the child's output, not the child
 * itself. Renaming would break the public API.
 */

/* pipe2() requires _GNU_SOURCE on Linux -- must be before any includes */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "exec_util.h"
#include "../nbs-common/nbs_assert.h"

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

/* Maximum number of argv entries before we consider the array unterminated */
#define ARGV_MAX_ENTRIES 256

/*
 * assert_argv_valid — Verify argv preconditions.
 *
 * Splits the compound assertion so each failure message is unambiguous.
 * Also verifies argv is NULL-terminated within ARGV_MAX_ENTRIES.
 */
static void assert_argv_valid(const char *const argv[], const char *func_name)
{
    ASSERT_MSG(argv != NULL,
               "%s: argv is NULL — caller passed no argument vector", func_name);
    ASSERT_MSG(argv[0] != NULL,
               "%s: argv[0] is NULL — no program name specified", func_name);

    /* Verify argv is NULL-terminated within reasonable bounds */
    int i;
    for (i = 0; i < ARGV_MAX_ENTRIES && argv[i] != NULL; i++) {}
    ASSERT_MSG(i < ARGV_MAX_ENTRIES,
               "%s: argv not NULL-terminated within %d entries",
               func_name, ARGV_MAX_ENTRIES);
}

/*
 * redirect_stderr_to_devnull — Redirect stderr to /dev/null in child.
 *
 * If /dev/null is unavailable (e.g. chroot), close stderr outright
 * so the child doesn't write to the parent's terminal.
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

int exec_capture(const char *const argv[], char *out_buf, size_t out_size)
{
    assert_argv_valid(argv, "exec_capture");
    ASSERT_MSG(out_buf != NULL,
               "exec_capture: out_buf is NULL — no buffer to capture into");
    ASSERT_MSG(out_size > 0,
               "exec_capture: out_size is 0 — buffer cannot hold even a NUL terminator");

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
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
        close(pipefd[0]); /* close read end */
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            close(pipefd[1]);
            _exit(126);
        }
        close(pipefd[1]);
        redirect_stderr_to_devnull();

        execvp(argv[0], (char *const *)argv);
        /* execvp only returns on failure — stderr is /dev/null, diagnostic lost */
        _exit(127);
    }

    /* Parent process */
    close(pipefd[1]); /* close write end */

    /* Read stdout from child into out_buf, leaving room for NUL */
    size_t total = 0;
    int read_error = 0;
    while (total < out_size - 1) {
        ssize_t n = read(pipefd[0], out_buf + total, out_size - 1 - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            read_error = errno;
            break;
        }
        if (n == 0)
            break; /* EOF */
        total += (size_t)n;
    }
    out_buf[total] = '\0';
    /* Postcondition: buffer is NUL-terminated and within bounds */
    ASSERT_MSG(total < out_size,
               "exec_capture postcondition: total %zu >= out_size %zu — "
               "NUL terminator would be out of bounds", total, out_size);
    close(pipefd[0]);

    /* Reap child — retry on EINTR */
    int status;
    pid_t wpid;
    do {
        wpid = waitpid(pid, &status, 0);
    } while (wpid < 0 && errno == EINTR);

    if (wpid < 0) {
        fprintf(stderr, "exec_capture: waitpid failed for pid %d: %s\n",
                (int)pid, strerror(errno));
        return -1;
    }

    /* If a read error occurred, return -1 regardless of child exit status.
     * Partial data in out_buf is NUL-terminated but should not be trusted. */
    if (read_error) {
        fprintf(stderr, "exec_capture: read from child pipe failed: %s\n",
                strerror(read_error));
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    /* Killed by signal or other abnormal termination */
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "exec_capture: child (pid %d) killed by signal %d\n",
                (int)pid, WTERMSIG(status));
    }
    return -1;
}

int exec_fire_and_forget(const char *const argv[])
{
    assert_argv_valid(argv, "exec_fire_and_forget");

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        /* Child process — redirect both stdout and stderr to /dev/null */
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
        /* execvp only returns on failure — stdout+stderr are /dev/null, diagnostic lost */
        _exit(127);
    }

    /* Parent: reap child — retry on EINTR */
    int status;
    pid_t wpid;
    do {
        wpid = waitpid(pid, &status, 0);
    } while (wpid < 0 && errno == EINTR);

    if (wpid < 0) {
        fprintf(stderr, "exec_fire_and_forget: waitpid failed for pid %d: %s\n",
                (int)pid, strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    /* Killed by signal or other abnormal termination */
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "exec_fire_and_forget: child (pid %d) killed by signal %d\n",
                (int)pid, WTERMSIG(status));
    }
    return -1;
}
