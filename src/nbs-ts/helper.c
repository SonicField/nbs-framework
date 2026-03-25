/*
 * helper.c — Centralised process launcher for nbs-ts sessions.
 *
 * Provides consistent PTY allocation, process-group management,
 * and audit logging for all nbs-ts sessions.
 *
 * Usage: nbs-ts-helper
 *
 * Listens on ~/.nbs-ts/helper.sock. For each connection:
 *   1. Verify peer credentials (SO_PEERCRED)
 *   2. Read command string
 *   3. openpty + fork + exec
 *   4. Send PTY master fd back to caller via SCM_RIGHTS
 *   5. Log the spawn
 *
 * Runs in foreground. Logs to stdout. Ctrl-C to stop.
 */

#define _GNU_SOURCE

#include "nbs_assert.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CMD_LEN 4096
#define SOCK_DIR_MODE 0700
#define SOCK_FILE_MODE 0600

static char g_sock_path[104];  /* must fit in sun_path (108 bytes) */
static volatile sig_atomic_t g_quit = 0;

static void handle_signal(int sig) {
    int saved_errno = errno;
    (void)sig;
    g_quit = 1;
    errno = saved_errno;
}

static void log_msg(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    printf("[%s] ", ts);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    printf("\n");
    fflush(stdout);
}

static int send_fd(int sock, int fd) {
    ASSERT_MSG(sock >= 0, "send_fd: invalid socket fd: %d", sock);
    ASSERT_MSG(fd >= 0, "send_fd: invalid fd to send: %d", fd);

    char buf[1] = {'F'};
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    return sendmsg(sock, &msg, 0) >= 0 ? 0 : -1;
}

static void reap_children(void) {
    /* Non-blocking wait for any exited children — log how they died */
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status)) {
            log_msg("child exited: pid %d exit_code=%d",
                    (int)pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            log_msg("child killed: pid %d signal=%d (%s)",
                    (int)pid, WTERMSIG(status), strsignal(WTERMSIG(status)));
        } else {
            log_msg("child reaped: pid %d raw_status=%d",
                    (int)pid, status);
        }
    }
}

static void handle_client(int client_fd) {
    ASSERT_MSG(client_fd >= 0, "handle_client: invalid client_fd: %d", client_fd);

    /* Verify peer credentials */
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED,
                   &cred, &cred_len) < 0) {
        log_msg("reject: SO_PEERCRED failed: %s", strerror(errno));
        close(client_fd);
        return;
    }

    if (cred.uid != getuid()) {
        log_msg("reject: uid mismatch (peer=%d, mine=%d, peer_pid=%d)",
                cred.uid, getuid(), cred.pid);
        close(client_fd);
        return;
    }

    /* Read command — loop until EOF (client closes after sending) */
    char cmd[MAX_CMD_LEN];
    size_t cmd_len = 0;
    for (;;) {
        ssize_t n = read(client_fd, cmd + cmd_len, sizeof(cmd) - 1 - cmd_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_msg("reject: read error: %s (peer_pid=%d)", strerror(errno), cred.pid);
            close(client_fd);
            return;
        }
        if (n == 0) break;  /* EOF — client closed write end */
        cmd_len += (size_t)n;
        if (cmd_len >= sizeof(cmd) - 1) break;
    }
    if (cmd_len == 0) {
        log_msg("reject: empty command (peer_pid=%d)", cred.pid);
        close(client_fd);
        return;
    }
    cmd[cmd_len] = '\0';

    /* Strip trailing newline if present */
    if (cmd_len > 0 && cmd[cmd_len - 1] == '\n') cmd[cmd_len - 1] = '\0';

    /* Create PTY */
    int master_fd, slave_fd;
    struct winsize ws = { .ws_row = 24, .ws_col = 80 };
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) < 0) {
        log_msg("error: openpty failed: %s (cmd=%s, peer_pid=%d)",
                strerror(errno), cmd, cred.pid);
        close(client_fd);
        return;
    }

    /* Fork child */
    pid_t child = fork();
    if (child < 0) {
        log_msg("error: fork failed: %s (cmd=%s, peer_pid=%d)",
                strerror(errno), cmd, cred.pid);
        close(master_fd);
        close(slave_fd);
        close(client_fd);
        return;
    }

    if (child == 0) {
        /* Child process */
        close(master_fd);
        close(client_fd);

        /* Reset SIGCHLD to default — the helper ignores it for reaping,
         * but the child's programs (git, ssh, etc.) need waitpid to work. */
        signal(SIGCHLD, SIG_DFL);

        setsid();
        ioctl(slave_fd, TIOCSCTTY, 0);

        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO)
            close(slave_fd);

        execlp("bash", "bash", "--login", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent — send master fd and child PID to caller */
    close(slave_fd);

    if (send_fd(client_fd, master_fd) < 0) {
        log_msg("error: send_fd failed: %s (cmd=%s, child=%d, peer_pid=%d)",
                strerror(errno), cmd, child, cred.pid);
    } else {
        /* Send child PID as a plain integer after the fd */
        char pid_buf[32];
        int pn = snprintf(pid_buf, sizeof(pid_buf), "%d", (int)child);
        if (pn > 0) write(client_fd, pid_buf, (size_t)pn);
        log_msg("spawn: \"%s\" -> pid %d (peer_pid=%d)",
                cmd, child, cred.pid);
    }

    close(master_fd);
    close(client_fd);
}

int main(void) {
    /* Build socket path */
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        fprintf(stderr, "nbs-ts-helper: HOME not set\n");
        return 1;
    }

    char dir_path[96];
    int dn = snprintf(dir_path, sizeof(dir_path), "%s/.nbs-ts", home);
    if (dn < 0 || (size_t)dn >= sizeof(dir_path)) {
        fprintf(stderr, "nbs-ts-helper: HOME too long for socket path\n");
        return 1;
    }
    int sn = snprintf(g_sock_path, sizeof(g_sock_path), "%s/helper.sock", dir_path);
    if (sn < 0 || (size_t)sn >= sizeof(g_sock_path)) {
        fprintf(stderr, "nbs-ts-helper: socket path too long\n");
        return 1;
    }

    /* Create directory */
    if (mkdir(dir_path, SOCK_DIR_MODE) < 0 && errno != EEXIST) {
        fprintf(stderr, "nbs-ts-helper: mkdir %s: %s\n",
                dir_path, strerror(errno));
        return 1;
    }
    chmod(dir_path, SOCK_DIR_MODE);

    /* Remove stale socket */
    unlink(g_sock_path);

    /* Create socket */
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        fprintf(stderr, "nbs-ts-helper: socket: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_sock_path);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "nbs-ts-helper: bind %s: %s\n",
                g_sock_path, strerror(errno));
        close(srv);
        return 1;
    }
    chmod(g_sock_path, SOCK_FILE_MODE);

    if (listen(srv, 16) < 0) {
        fprintf(stderr, "nbs-ts-helper: listen: %s\n", strerror(errno));
        close(srv);
        unlink(g_sock_path);
        return 1;
    }

    /* Signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* SIGCHLD: use SA_NOCLDSTOP so stopped children don't wake us,
     * but leave the default handler so zombies accumulate for
     * reap_children() to collect via waitpid(WNOHANG). */
    {
        struct sigaction sa_chld;
        memset(&sa_chld, 0, sizeof(sa_chld));
        sa_chld.sa_handler = SIG_DFL;
        sa_chld.sa_flags = SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa_chld, NULL);
    }

    log_msg("nbs-ts-helper started (pid %d)", getpid());
    log_msg("Listening on %s", g_sock_path);

    /* Main loop */
    while (!g_quit) {
        reap_children();

        /* Use select with timeout so we check g_quit periodically */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int ready = select(srv + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) continue;

        int client_fd = accept(srv, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            log_msg("error: accept: %s", strerror(errno));
            continue;
        }

        handle_client(client_fd);
    }

    /* Cleanup */
    close(srv);
    unlink(g_sock_path);
    log_msg("nbs-ts-helper stopped");
    return 0;
}
