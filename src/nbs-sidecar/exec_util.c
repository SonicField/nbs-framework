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
 */

#include "exec_util.h"
#include "../nbs-common/nbs_assert.h"

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

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
        close(pipefd[0]); /* close read end */
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            close(pipefd[1]);
            _exit(126);
        }
        close(pipefd[1]);
        redirect_stderr_to_devnull();

        execvp(argv[0], (char *const *)argv);
        /* execvp only returns on failure — errno must be set */
        fprintf(stderr, "exec failed: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    /* Parent process */
    close(pipefd[1]); /* close write end */

    /* Read stdout from child into out_buf, leaving room for NUL */
    size_t total = 0;
    while (total < out_size - 1) {
        ssize_t n = read(pipefd[0], out_buf + total, out_size - 1 - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break; /* read error */
        }
        if (n == 0)
            break; /* EOF */
        total += (size_t)n;
    }
    out_buf[total] = '\0';
    /* Postcondition: buffer is NUL-terminated and within bounds */
    ASSERT_MSG(total < out_size, "exec_capture: total %zu >= out_size %zu", total, out_size);
    close(pipefd[0]);

    /* Reap child — retry on EINTR */
    int status;
    pid_t wpid;
    do {
        wpid = waitpid(pid, &status, 0);
    } while (wpid < 0 && errno == EINTR);

    if (wpid < 0) {
        /* ECHILD or other error */
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    /* Killed by signal or other abnormal termination */
    return -1;
}

int exec_fire_and_forget(const char *const argv[])
{
    ASSERT_MSG(argv != NULL && argv[0] != NULL,
               "exec_fire_and_forget: argv or argv[0] is NULL");

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
        /* execvp only returns on failure — errno must be set */
        fprintf(stderr, "exec failed: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    /* Parent: reap child — retry on EINTR */
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
