/*
 * transport_ts.c — nbs-ts transport implementation.
 *
 * Implements the transport vtable for nbs-ts managed sessions.
 * Uses direct file I/O against the session directory — no fork+exec.
 *
 * Session directory layout (stable interface):
 *   ~/.nbs-ts/sessions/<handle>/output.log   — append-only output
 *   ~/.nbs-ts/sessions/<handle>/input.fifo   — write to send input
 *   ~/.nbs-ts/sessions/<handle>/pid          — child process PID
 */

/* H5 fix: removed blanket -Wformat-truncation suppression.
 * All snprintf calls in this file check return values and handle
 * truncation explicitly — the pragma was masking potential issues. */

#include "transport.h"
#include "../nbs-common/nbs_assert.h"
#include "nbs_ts_render.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>

#define TS_CAPTURE_BUF_SIZE 32768
#define TS_MAX_PATH 4096

/* Debug logging — writes to /tmp/nbs-sidecar-debug-<pid>.log when
 * NBS_DEBUG=1 is set. Survives >/dev/null 2>&1 because it writes
 * to a file, not stderr. */
static FILE *g_ts_debug_fp = NULL;
static int g_ts_debug_checked = 0;

static void ts_dbg(const char *fmt, ...) {
    if (!g_ts_debug_checked) {
        g_ts_debug_checked = 1;
        /* Always log — temporary, for debugging oracle death */
        char path[256];
        snprintf(path, sizeof(path), "/tmp/nbs-sidecar-debug-%d.log",
                 (int)getpid());
        g_ts_debug_fp = fopen(path, "a");
    }
    if (!g_ts_debug_fp) return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(g_ts_debug_fp, "[sidecar:%d] %02d:%02d:%02d ",
            (int)getpid(), tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_ts_debug_fp, fmt, ap);
    va_end(ap);
    fprintf(g_ts_debug_fp, "\n");
    fflush(g_ts_debug_fp);
}

/* Context for nbs-ts transport */
typedef struct {
    char session_dir[TS_MAX_PATH];
    char output_log[TS_MAX_PATH];
    char input_fifo[TS_MAX_PATH];
    char pid_path[TS_MAX_PATH];
} ts_ctx_t;

/*
 * resolve_session_dir — Build ~/.nbs-ts/sessions/<handle> path.
 * Self-contained — no dependency on nbs-ts library.
 */
static int resolve_session_dir(const char *handle, char *buf, size_t bufsize)
{
    ASSERT_MSG(handle != NULL, "resolve_session_dir: handle is NULL");
    ASSERT_MSG(buf != NULL, "resolve_session_dir: buf is NULL");
    ASSERT_MSG(bufsize > 0, "resolve_session_dir: bufsize is 0");

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') return -1;

    int n = snprintf(buf, bufsize, "%s/.nbs-ts/sessions/%s", home, handle);
    if (n < 0 || (size_t)n >= bufsize) return -1;
    return 0;
}

/*
 * read_pid — Read the PID from the session's pid file.
 * Returns the PID, or -1 on error.
 */
static pid_t read_pid(const ts_ctx_t *ctx)
{
    ASSERT_MSG(ctx != NULL, "read_pid: ctx is NULL");

    int fd = open(ctx->pid_path, O_RDONLY);
    if (fd < 0) return -1;

    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;

    buf[n] = '\0';
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';

    errno = 0;
    char *endptr;
    long val = strtol(buf, &endptr, 10);
    if (errno != 0 || endptr == buf || *endptr != '\0') return -1;
    /* B4 fix: reject pid=0 (kernel) and negative values (invalid) */
    if (val <= 0) return -1;
    return (pid_t)val;
}

/*
 * find_last_screen_clear — Scan backwards for ESC[2J in a buffer.
 *
 * Returns the byte offset of the last screen clear, or -1 if not found.
 * max_scan limits how far back to search.
 */
static off_t find_last_screen_clear(int fd, off_t file_end, off_t max_scan)
{
    if (file_end <= 0) return -1;
    if (max_scan > file_end) max_scan = file_end;

    off_t scan_pos = file_end - max_scan;
    char *scan_buf = malloc((size_t)max_scan);
    if (!scan_buf) return -1;

    ssize_t nr = pread(fd, scan_buf, (size_t)max_scan, scan_pos);
    off_t result = -1;
    if (nr > 3) {
        for (ssize_t i = nr - 4; i >= 0; i--) {
            if (scan_buf[i] == '\x1b' && scan_buf[i+1] == '[' &&
                scan_buf[i+2] == '2' && scan_buf[i+3] == 'J') {
                result = scan_pos + i;
                break;
            }
        }
    }
    free(scan_buf);
    return result;
}

/*
 * ts_capture — Render the terminal screen from output.log.
 *
 * Feeds raw PTY output through the terminal renderer to produce the
 * actual screen content (cursor movement, scrolling, erase resolved).
 * The scrollback parameter is unused — the renderer always returns
 * the full terminal screen. Kept for vtable compatibility.
 *
 * Returns heap-allocated NUL-terminated string, or NULL on error.
 * Caller must free().
 */
static char *ts_capture(const transport_t *self, int scrollback)
{
    ASSERT_MSG(self != NULL, "ts_capture: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "ts_capture: ctx is NULL");
    const ts_ctx_t *ctx = self->ctx;
    (void)scrollback;

    int fd = open(ctx->output_log, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ts_capture: cannot open '%s': %s\n",
                ctx->output_log, strerror(errno));
        return NULL;
    }

    off_t file_end = lseek(fd, 0, SEEK_END);
    if (file_end <= 0) {
        close(fd);
        char *empty = calloc(1, 1);
        ASSERT_MSG(empty != NULL, "ts_capture: calloc(1,1) failed — out of memory");
        return empty;
    }

    off_t render_start = find_last_screen_clear(fd, file_end, 1024 * 1024);
    if (render_start < 0 && file_end > 65536)
        render_start = file_end - 65536;
    if (render_start < 0)
        render_start = 0;

    ts_render_t *renderer = ts_render_create(24, 80);
    if (!renderer) {
        fprintf(stderr, "ts_capture: failed to allocate renderer\n");
        close(fd);
        return NULL;
    }

    off_t pos = render_start;
    char feed_buf[65536];
    while (pos < file_end) {
        size_t chunk = sizeof(feed_buf);
        if ((off_t)chunk > file_end - pos)
            chunk = (size_t)(file_end - pos);
        ssize_t nr = pread(fd, feed_buf, chunk, pos);
        if (nr <= 0) break;
        ts_render_feed(renderer, feed_buf, (size_t)nr);
        pos += nr;
    }
    close(fd);

    char *result = ts_render_snapshot(renderer);
    ts_render_destroy(renderer);

    if (!result) {
        char *empty = calloc(1, 1);
        ASSERT_MSG(empty != NULL, "ts_capture: calloc(1,1) failed — out of memory");
        return empty;
    }

    return result;
}

/*
 * ts_send_text — Write text to the session's input FIFO.
 *
 * No trailing Enter — the vtable contract says send_text does not
 * append a newline. Use send_key("Enter") for that.
 */
static int ts_send_text(const transport_t *self, const char *text)
{
    ASSERT_MSG(self != NULL, "ts_send_text: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "ts_send_text: ctx is NULL");
    ASSERT_MSG(text != NULL, "ts_send_text: text is NULL");
    const ts_ctx_t *ctx = self->ctx;

    /* Log first 80 chars of text being sent */
    {
        char preview[81];
        size_t plen = strlen(text);
        if (plen > 80) plen = 80;
        memcpy(preview, text, plen);
        preview[plen] = '\0';
        ts_dbg("send_text: len=%zu preview=\"%.80s\"", strlen(text), preview);
    }

    int fd = open(ctx->input_fifo, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "ts_send_text: cannot open FIFO '%s': %s\n",
                ctx->input_fifo, strerror(errno));
        ts_dbg("send_text: FIFO open FAILED: %s", strerror(errno));
        return -1;
    }

    /* Clear O_NONBLOCK after open so writes block until complete */
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        fprintf(stderr, "ts_send_text: fcntl F_GETFL failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        fprintf(stderr, "ts_send_text: fcntl F_SETFL failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }

    /* Wrap content in bracketed paste markers so TUI apps (like Claude
     * Code) that enable bracketed paste mode ([?2004h]) treat the input
     * as a complete paste unit rather than individual keystrokes. Without
     * these markers, Enter is treated as newline-in-editor, not submit. */
    static const char paste_start[] = "\x1b[200~";
    static const char paste_end[] = "\x1b[201~";

    if (write(fd, paste_start, sizeof(paste_start) - 1) < 0) {
        fprintf(stderr, "ts_send_text: write paste_start failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }

    size_t len = strlen(text);
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, text + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "ts_send_text: write failed: %s\n",
                    strerror(errno));
            close(fd);
            return -1;
        }
        written += (size_t)w;
    }

    if (write(fd, paste_end, sizeof(paste_end) - 1) < 0) {
        fprintf(stderr, "ts_send_text: write paste_end failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

/*
 * ts_send_key — Send a named key to the session.
 *
 * "Enter" → '\r' (CR), "Escape" → '\x1b'. Direct byte injection.
 */
static int ts_send_key(const transport_t *self, const char *key)
{
    ASSERT_MSG(self != NULL, "ts_send_key: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "ts_send_key: ctx is NULL");
    ASSERT_MSG(key != NULL, "ts_send_key: key is NULL");
    const ts_ctx_t *ctx = self->ctx;

    const char *data;
    size_t len;

    ts_dbg("send_key: \"%s\"", key);

    if (strcmp(key, "Enter") == 0) {
        data = "\r";  /* CR, not LF — raw terminal mode expects 0x0d */
        len = 1;
    } else if (strcmp(key, "Escape") == 0) {
        data = "\x1b";
        len = 1;
    } else {
        data = key;
        len = strlen(key);
    }

    int fd = open(ctx->input_fifo, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "ts_send_key: cannot open FIFO '%s': %s\n",
                ctx->input_fifo, strerror(errno));
        return -1;
    }

    int sflags = fcntl(fd, F_GETFL);
    if (sflags < 0) {
        fprintf(stderr, "ts_send_key: fcntl F_GETFL failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }
    if (fcntl(fd, F_SETFL, sflags & ~O_NONBLOCK) < 0) {
        fprintf(stderr, "ts_send_key: fcntl F_SETFL failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }

    ssize_t w = write(fd, data, len);
    close(fd);

    if (w < 0 || (size_t)w != len) {
        fprintf(stderr, "ts_send_key: write failed for key '%s': %s\n",
                key, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * ts_is_alive — Check if the session's child process is alive.
 *
 * Returns: 1 if alive, 0 if gone, -1 on error.
 */
static int ts_is_alive(const transport_t *self)
{
    ASSERT_MSG(self != NULL, "ts_is_alive: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "ts_is_alive: ctx is NULL");
    const ts_ctx_t *ctx = self->ctx;

    pid_t pid = read_pid(ctx);
    if (pid <= 0) {
        ts_dbg("is_alive: cannot read pid");
        return -1;
    }

    if (kill(pid, 0) == 0) return 1;
    if (errno == ESRCH) {
        ts_dbg("is_alive: pid %d DEAD (ESRCH)", (int)pid);
        return 0;
    }
    ts_dbg("is_alive: pid %d kill(0) error: %s", (int)pid, strerror(errno));
    return -1;
}

/*
 * transport_ts_init — Initialise an nbs-ts transport.
 *
 * handle: 8-char hex session handle from nbs-ts create.
 *
 * Resolves the session directory and sets up direct file I/O paths.
 * No fork+exec — all operations are syscalls against the session files.
 */
int transport_ts_init(transport_t *tp, const char *handle)
{
    ASSERT_MSG(tp != NULL, "transport_ts_init: tp is NULL");
    ASSERT_MSG(handle != NULL, "transport_ts_init: handle is NULL");

    memset(tp, 0, sizeof(*tp));

    if (handle[0] == '\0') {
        fprintf(stderr, "transport_ts_init: handle is empty\n");
        return -1;
    }

    /* S2 fix: reject path traversal in session handle.
     * Handle is used to construct ~/.nbs-ts/sessions/<handle>/ — a handle
     * containing '/' or '..' could escape the sessions directory. */
    if (strchr(handle, '/') != NULL || strstr(handle, "..") != NULL) {
        fprintf(stderr, "transport_ts_init: handle contains path traversal "
                "characters: '%s'\n", handle);
        return -1;
    }

    /* Resolve session directory */
    char session_dir[TS_MAX_PATH];
    if (resolve_session_dir(handle, session_dir, sizeof(session_dir)) < 0) {
        fprintf(stderr, "transport_ts_init: cannot resolve session dir "
                "for handle '%s'\n", handle);
        return -1;
    }

    /* Verify session exists */
    struct stat st;
    if (stat(session_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "transport_ts_init: session dir '%s' does not exist\n",
                session_dir);
        return -1;
    }

    ts_ctx_t *ctx = calloc(1, sizeof(ts_ctx_t));
    if (!ctx) {
        fprintf(stderr, "transport_ts_init: calloc(%zu) failed — out of memory\n",
                sizeof(ts_ctx_t));
        return -1;
    }

    int r;
    r = snprintf(ctx->session_dir, sizeof(ctx->session_dir), "%s", session_dir);
    if (r < 0 || (size_t)r >= sizeof(ctx->session_dir)) {
        fprintf(stderr, "transport_ts_init: session_dir path truncated\n");
        free(ctx);
        return -1;
    }
    r = snprintf(ctx->output_log, sizeof(ctx->output_log),
                 "%s/output.log", session_dir);
    if (r < 0 || (size_t)r >= sizeof(ctx->output_log)) {
        fprintf(stderr, "transport_ts_init: output_log path truncated\n");
        free(ctx);
        return -1;
    }
    r = snprintf(ctx->input_fifo, sizeof(ctx->input_fifo),
                 "%s/input.fifo", session_dir);
    if (r < 0 || (size_t)r >= sizeof(ctx->input_fifo)) {
        fprintf(stderr, "transport_ts_init: input_fifo path truncated\n");
        free(ctx);
        return -1;
    }
    r = snprintf(ctx->pid_path, sizeof(ctx->pid_path),
                 "%s/pid", session_dir);
    if (r < 0 || (size_t)r >= sizeof(ctx->pid_path)) {
        fprintf(stderr, "transport_ts_init: pid_path path truncated\n");
        free(ctx);
        return -1;
    }

    tp->capture = ts_capture;
    tp->send_text = ts_send_text;
    tp->send_key = ts_send_key;
    tp->is_alive = ts_is_alive;
    tp->ctx = ctx;

    /* Postcondition: all vtable entries set */
    ASSERT_MSG(tp->capture != NULL && tp->send_text != NULL &&
               tp->send_key != NULL && tp->is_alive != NULL && tp->ctx != NULL,
               "transport_ts_init: postcondition violated - NULL vtable entry");
    return 0;
}

void transport_free(transport_t *tp)
{
    if (!tp) return;

    int all_null = (tp->capture == NULL && tp->send_text == NULL &&
                    tp->send_key == NULL && tp->is_alive == NULL);
    int all_set = (tp->capture != NULL && tp->send_text != NULL &&
                   tp->send_key != NULL && tp->is_alive != NULL);
    ASSERT_MSG(all_null || all_set,
               "transport_free: partially initialised transport — corrupt state");

    free(tp->ctx);
    memset(tp, 0, sizeof(*tp));
}
