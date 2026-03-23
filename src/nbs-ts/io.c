/*
 * io.c — Send input and read output from nbs-ts sessions.
 *
 * send: direct write(2) to PTY master fd. No keystroke simulation.
 * read: pread(2) from the append-only output log.
 *
 * Invariants:
 *   - write() retries on EINTR
 *   - pread() from output.log is always safe (append-only, never truncated)
 *   - read_new advances the session's read cursor
 */

#include "session_internal.h"
#include "nbs_assert.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>

int nbs_ts_send(nbs_ts_session_t *s, const char *data, size_t len)
{
    ASSERT_MSG(s != NULL, "nbs_ts_send: session is NULL");
    ASSERT_MSG(data != NULL, "nbs_ts_send: data is NULL");

    if (s->master_fd < 0) return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t w = write(s->master_fd, data + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += (size_t)w;
    }

    return 0;
}

size_t nbs_ts_read_new(nbs_ts_session_t *s, char *buf, size_t max_len)
{
    ASSERT_MSG(s != NULL, "nbs_ts_read_new: session is NULL");
    ASSERT_MSG(buf != NULL, "nbs_ts_read_new: buf is NULL");

    int fd = open(s->output_log_path, O_RDONLY);
    if (fd < 0) return 0;

    ssize_t n = pread(fd, buf, max_len, s->read_cursor);
    close(fd);

    if (n <= 0) return 0;

    s->read_cursor += n;
    return (size_t)n;
}

size_t nbs_ts_read(nbs_ts_session_t *s, char *buf, size_t max_len, off_t offset)
{
    ASSERT_MSG(s != NULL, "nbs_ts_read: session is NULL");
    ASSERT_MSG(buf != NULL, "nbs_ts_read: buf is NULL");

    int fd = open(s->output_log_path, O_RDONLY);
    if (fd < 0) return 0;

    ssize_t n = pread(fd, buf, max_len, offset);
    close(fd);

    return (n > 0) ? (size_t)n : 0;
}

/*
 * nbs_ts_read_tail — Read the last n_lines lines from the output log.
 *
 * Reads backwards from the end of the file to find line boundaries.
 * Returns the number of bytes written to buf. The output is NOT
 * NUL-terminated (caller must use the return value for length).
 */
size_t nbs_ts_read_tail(nbs_ts_session_t *s, char *buf, size_t max_len,
                        int n_lines)
{
    ASSERT_MSG(s != NULL, "nbs_ts_read_tail: session is NULL");
    ASSERT_MSG(buf != NULL, "nbs_ts_read_tail: buf is NULL");
    ASSERT_MSG(n_lines > 0, "nbs_ts_read_tail: n_lines must be positive");

    int fd = open(s->output_log_path, O_RDONLY);
    if (fd < 0) return 0;

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { close(fd); return 0; }

    /* Read the tail portion of the file (up to max_len) */
    size_t read_size = (size_t)file_size;
    if (read_size > max_len) read_size = max_len;

    off_t read_offset = file_size - (off_t)read_size;
    char *tmp = malloc(read_size);
    if (!tmp) { close(fd); return 0; }

    ssize_t n = pread(fd, tmp, read_size, read_offset);
    close(fd);
    if (n <= 0) { free(tmp); return 0; }

    /* Scan backwards to find the start of the last n_lines lines */
    int lines_found = 0;
    ssize_t pos = n - 1;

    /* Skip trailing newline */
    if (pos >= 0 && tmp[pos] == '\n') pos--;

    while (pos >= 0 && lines_found < n_lines) {
        if (tmp[pos] == '\n') lines_found++;
        if (lines_found < n_lines) pos--;
    }

    /* pos is at the newline before the first wanted line, or -1 */
    ssize_t start = (pos < 0) ? 0 : pos + 1;
    size_t result_len = (size_t)(n - start);
    if (result_len > max_len) result_len = max_len;

    memcpy(buf, tmp + start, result_len);
    free(tmp);
    return result_len;
}
