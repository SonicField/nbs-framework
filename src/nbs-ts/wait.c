/*
 * wait.c — inotify-based wait operations for nbs-ts sessions.
 *
 * wait_complete: watches completion.log for new records.
 * wait_pattern: watches output.log for a string match.
 *
 * Both use inotify(7) for event-driven notification — no polling.
 * Timeout is enforced via poll(2) with millisecond precision.
 *
 * Invariants:
 *   - inotify fd and watch are always cleaned up on all paths
 *   - Timeout uses CLOCK_MONOTONIC, not accumulated sleep
 *   - Pattern matching is plain strstr (no regex)
 */

#include "session_internal.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/inotify.h>

static long long get_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "get_monotonic_ms: clock_gettime failed: errno=%d\n",
                errno);
        return -1;
    }
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int nbs_ts_wait_complete(nbs_ts_session_t *s, int timeout_ms,
                         nbs_ts_completion_t *out)
{
    ASSERT_MSG(s != NULL, "nbs_ts_wait_complete: session is NULL");
    ASSERT_MSG(out != NULL, "nbs_ts_wait_complete: out is NULL");
    ASSERT_MSG(timeout_ms >= 0, "nbs_ts_wait_complete: timeout_ms must be "
               "non-negative, got %d", timeout_ms);

    unsigned long cursor = s->completion_cursor;

    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) {
        fprintf(stderr, "nbs_ts_wait_complete: inotify_init1 failed: "
                "errno=%d\n", errno);
        return -1;
    }

    int wd = inotify_add_watch(ifd, s->completion_log_path,
                               IN_MODIFY | IN_CLOSE_WRITE);
    if (wd < 0) {
        fprintf(stderr, "nbs_ts_wait_complete: inotify_add_watch failed: "
                "path=%s errno=%d\n", s->completion_log_path, errno);
        close(ifd);
        return -1;
    }

    long long deadline = get_monotonic_ms() + timeout_ms;
    int found = 0;

    for (;;) {
        FILE *f = fopen(s->completion_log_path, "r");
        if (f) {
            char line[128];
            unsigned long seq;
            int code;
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%lu %d", &seq, &code) == 2) {
                    if (seq > cursor) {
                        out->seq = seq;
                        out->exit_code = code;
                        s->completion_cursor = seq;
                        found = 1;
                    }
                }
            }
            fclose(f);
        }

        if (found) break;

        long long now = get_monotonic_ms();
        int remaining = (int)(deadline - now);
        if (remaining <= 0) break;

        struct pollfd pfd = { .fd = ifd, .events = POLLIN };
        int pr = poll(&pfd, 1, remaining);

        if (pr > 0) {
            char evbuf[256];
            while (read(ifd, evbuf, sizeof(evbuf)) > 0)
                ;
        } else if (pr == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);

    return found ? 0 : -1;
}

int nbs_ts_wait_pattern(nbs_ts_session_t *s, const char *pattern,
                        int timeout_ms)
{
    ASSERT_MSG(s != NULL, "nbs_ts_wait_pattern: session is NULL");
    ASSERT_MSG(pattern != NULL, "nbs_ts_wait_pattern: pattern is NULL");
    ASSERT_MSG(pattern[0] != '\0', "nbs_ts_wait_pattern: pattern is empty");
    ASSERT_MSG(timeout_ms >= 0, "nbs_ts_wait_pattern: timeout_ms must be "
               "non-negative, got %d", timeout_ms);

    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) {
        fprintf(stderr, "nbs_ts_wait_pattern: inotify_init1 failed: "
                "errno=%d\n", errno);
        return -1;
    }

    int wd = inotify_add_watch(ifd, s->output_log_path,
                               IN_MODIFY | IN_CLOSE_WRITE);
    if (wd < 0) {
        fprintf(stderr, "nbs_ts_wait_pattern: inotify_add_watch failed: "
                "path=%s errno=%d\n", s->output_log_path, errno);
        close(ifd);
        return -1;
    }

    long long deadline = get_monotonic_ms() + timeout_ms;
    int found = 0;
    size_t search_offset = 0;

    for (;;) {
        int fd = open(s->output_log_path, O_RDONLY);
        if (fd >= 0) {
            off_t end = lseek(fd, 0, SEEK_END);
            if (end > (off_t)search_offset) {
                size_t to_read = (size_t)(end - (off_t)search_offset);
                if (to_read > 1024 * 1024) to_read = 1024 * 1024;

                char *buf = malloc(to_read + 1);
                if (buf) {
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

        long long now = get_monotonic_ms();
        int remaining = (int)(deadline - now);
        if (remaining <= 0) break;

        struct pollfd pfd = { .fd = ifd, .events = POLLIN };
        int pr = poll(&pfd, 1, remaining);

        if (pr > 0) {
            char evbuf[256];
            while (read(ifd, evbuf, sizeof(evbuf)) > 0)
                ;
        } else if (pr == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);

    return found ? 0 : -1;
}
