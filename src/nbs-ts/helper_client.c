/*
 * helper_client.c — Connect to nbs-ts-helper and request a PTY.
 *
 * Returns the PTY master fd on success, -1 if helper is not running.
 */

#include "helper_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CMD_LEN 4096

int helper_request_pty(const char *command) {
    if (!command || command[0] == '\0') return -1;

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
    if (write(s, command, cmd_len) < 0) {
        close(s);
        return -1;
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

    ssize_t n = recvmsg(s, &msg, 0);
    close(s);

    if (n <= 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg ||
        cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len < CMSG_LEN(sizeof(int))) {
        return -1;
    }

    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}
