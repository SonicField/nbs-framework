/*
 * helper_client.c — Connect to nbs-ts-helper and request a PTY.
 *
 * Returns the PTY master fd on success, -1 if helper is not running.
 */

#include "helper_client.h"
#include "nbs_assert.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CMD_LEN 4096

int helper_request_pty(const char *command, pid_t *out_child_pid) {
    ASSERT_MSG(command != NULL, "helper_request_pty: command is NULL");
    ASSERT_MSG(command[0] != '\0', "helper_request_pty: command is empty");


    /* Build socket path */
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') return -1;

    char sock_path[104];  /* sun_path is 108 bytes max */
    int sn = snprintf(sock_path, sizeof(sock_path), "%s/.nbs-ts/helper.sock", home);
    if (sn < 0 || (size_t)sn >= sizeof(sock_path)) return -1;  /* HOME too long */

    /* Connect */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "nbs-ts: helper connect failed (%s): %s\n",
                sock_path, strerror(errno));
        close(s);
        return -1;  /* Helper not running */
    }

    /* Send command */
    size_t cmd_len = strlen(command);
    if (cmd_len >= MAX_CMD_LEN) {
        close(s);
        return -1;
    }
    {
        size_t written = 0;
        while (written < cmd_len) {
            ssize_t w = write(s, command + written, cmd_len - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                close(s);
                return -1;
            }
            written += (size_t)w;
        }
    }

    /* Receive fd via SCM_RIGHTS */
    char buf[1];
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

    /* Signal EOF to helper so its read loop terminates */
    shutdown(s, SHUT_WR);

    ssize_t n = recvmsg(s, &msg, 0);

    if (n <= 0) { close(s); return -1; }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg ||
        cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len < CMSG_LEN(sizeof(int))) {
        close(s);
        return -1;
    }

    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));

    /* Read child PID sent after the fd */
    if (out_child_pid) {
        char pid_buf[32];
        ssize_t pr = read(s, pid_buf, sizeof(pid_buf) - 1);
        if (pr > 0) {
            pid_buf[pr] = '\0';
            char *endptr;
            long val = strtol(pid_buf, &endptr, 10);
            *out_child_pid = (val > 0 && *endptr == '\0') ? (pid_t)val : 0;
        } else {
            *out_child_pid = 0;
        }
    }

    close(s);
    return fd;
}
