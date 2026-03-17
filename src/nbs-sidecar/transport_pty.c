/*
 * transport_pty.c -- pty-session transport implementation.
 *
 * Implements the transport vtable for pty-session managed sessions.
 * Each operation is a fork+exec of the pty-session binary.
 */

#include "transport.h"
#include "exec_util.h"
#include "../nbs-common/nbs_assert.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Named constant for capture buffer size (Violation 1: HARDENING) */
#define PTY_CAPTURE_BUF_SIZE 32768

/* Named constants for context buffer sizes */
#define PTY_PATH_MAX 4096
#define PTY_SESSION_MAX 256

/* Context for pty-session transport */
typedef struct {
    char pty_path[PTY_PATH_MAX];
    char session_name[PTY_SESSION_MAX];
} pty_ctx_t;

static char *pty_capture(const transport_t *self, int scrollback) {
    ASSERT_MSG(self != NULL, "pty_capture: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "pty_capture: ctx is NULL");
    const pty_ctx_t *ctx = self->ctx;

    ASSERT_MSG(scrollback >= 0, "pty_capture: scrollback must be non-negative, got %d", scrollback);
    char scroll_arg[32];
    snprintf(scroll_arg, sizeof(scroll_arg), "--scrollback=%d", scrollback);

    const char *argv[] = {
        ctx->pty_path, "read", ctx->session_name, scroll_arg, NULL
    };

    char *buf = malloc(PTY_CAPTURE_BUF_SIZE);
    if (!buf) {
        fprintf(stderr, "pty_capture: malloc(%d) failed for session '%s'\n",
                PTY_CAPTURE_BUF_SIZE, ctx->session_name);
        return NULL;
    }

    int rc = exec_capture(argv, buf, PTY_CAPTURE_BUF_SIZE);
    if (rc < 0) {
        /* Violation 2 (BUG): log exec failure instead of silent discard */
        fprintf(stderr, "pty_capture: exec_capture failed (rc=%d) for session '%s'\n",
                rc, ctx->session_name);
        free(buf);
        return NULL;
    }

    /* Postcondition: buffer is NUL-terminated within bounds */
    ASSERT_MSG(memchr(buf, '\0', PTY_CAPTURE_BUF_SIZE) != NULL,
               "pty_capture: buffer not NUL-terminated after exec_capture");
    return buf;
}

static int pty_send_text(const transport_t *self, const char *text) {
    ASSERT_MSG(self != NULL, "pty_send_text: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "pty_send_text: ctx is NULL");
    ASSERT_MSG(text != NULL, "pty_send_text: text is NULL");
    const pty_ctx_t *ctx = self->ctx;

    const char *argv[] = {
        ctx->pty_path, "send", ctx->session_name, text, NULL
    };

    /* Violation 4 (BUG): log exec failure instead of silent discard */
    int rc = exec_fire_and_forget(argv);
    if (rc != 0) {
        fprintf(stderr, "pty_send_text: exec failed (rc=%d) for session '%s'\n",
                rc, ctx->session_name);
        return -1;
    }
    return 0;
}

static int pty_send_key(const transport_t *self, const char *key) {
    ASSERT_MSG(self != NULL, "pty_send_key: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "pty_send_key: ctx is NULL");
    ASSERT_MSG(key != NULL, "pty_send_key: key is NULL");
    const pty_ctx_t *ctx = self->ctx;

    const char *argv_enter[] = {
        ctx->pty_path, "send", ctx->session_name, "", NULL
    };
    const char *argv_escape[] = {
        ctx->pty_path, "send", ctx->session_name, "--no-enter", "\x1b", NULL
    };
    const char *argv_other[] = {
        ctx->pty_path, "send", ctx->session_name, "--no-enter", key, NULL
    };

    const char *const *argv;
    if (strcmp(key, "Enter") == 0) {
        argv = argv_enter;
    } else if (strcmp(key, "Escape") == 0) {
        argv = argv_escape;
    } else {
        argv = argv_other;
    }

    /* Violation 5 (BUG): log exec failure instead of silent discard */
    int rc = exec_fire_and_forget(argv);
    if (rc != 0) {
        fprintf(stderr, "pty_send_key: exec failed (rc=%d) for session '%s', key '%s'\n",
                rc, ctx->session_name, key);
        return -1;
    }
    return 0;
}

static int pty_is_alive(const transport_t *self) {
    ASSERT_MSG(self != NULL, "pty_is_alive: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "pty_is_alive: ctx is NULL");
    const pty_ctx_t *ctx = self->ctx;

    char buf[PTY_PATH_MAX];
    const char *argv[] = {
        ctx->pty_path, "list", NULL
    };

    int rc = exec_capture(argv, buf, sizeof(buf));
    /* Postcondition: rc must be less than buffer size (no truncation) */
    ASSERT_MSG(rc < (int)sizeof(buf),
               "pty_is_alive: exec_capture output truncated (rc=%d, buf=%zu)",
               rc, sizeof(buf));
    /* Violation 6 (BUG): distinguish exec error from "session not found" */
    if (rc < 0) {
        fprintf(stderr, "pty_is_alive: exec_capture failed (rc=%d) for session '%s'\n",
                rc, ctx->session_name);
        return -1;  /* error, not "dead" */
    }
    if (rc != 0) return 0;  /* command ran but session not listed */

    /* Violation 7 (SECURITY): defensive NUL-termination before strtok */
    buf[sizeof(buf) - 1] = '\0';

    /* Check if session_name appears as a complete line in the output */
    char *line = strtok(buf, "\n");
    while (line) {
        /* Trim leading whitespace */
        while (*line == ' ' || *line == '\t') line++;
        size_t len = strlen(line);

        /* Violation 8 (HARDENING): skip empty tokens explicitly */
        if (len == 0) {
            line = strtok(NULL, "\n");
            continue;
        }

        /* Trim trailing whitespace */
        char *end = line + len - 1;
        while (end > line && (*end == ' ' || *end == '\t' || *end == '\r')) {
            *end = '\0';
            end--;
        }

        if (strcmp(line, ctx->session_name) == 0) {
            return 1;
        }
        line = strtok(NULL, "\n");
    }

    return 0;
}

int transport_pty_init(transport_t *tp, const char *pty_path,
                       const char *session_name) {
    ASSERT_MSG(tp != NULL, "transport_pty_init: tp is NULL");
    ASSERT_MSG(pty_path != NULL, "transport_pty_init: pty_path is NULL");
    ASSERT_MSG(session_name != NULL, "transport_pty_init: session_name is NULL");

    memset(tp, 0, sizeof(*tp));

    /* Violation 9 (SECURITY): external input length checks return -1, not abort.
     * pty_path and session_name originate from user/environment input. */
    if (pty_path[0] == '\0') {
        fprintf(stderr, "transport_pty_init: pty_path is empty\n");
        return -1;
    }
    if (session_name[0] == '\0') {
        fprintf(stderr, "transport_pty_init: session_name is empty\n");
        return -1;
    }

    /* Violation 12 (HARDENING): cache strlen to avoid double evaluation */
    size_t path_len = strlen(pty_path);
    size_t name_len = strlen(session_name);

    if (path_len >= sizeof(((pty_ctx_t*)0)->pty_path)) {
        fprintf(stderr, "transport_pty_init: pty_path too long (%zu >= %zu)\n",
                path_len, sizeof(((pty_ctx_t*)0)->pty_path));
        return -1;
    }
    if (name_len >= sizeof(((pty_ctx_t*)0)->session_name)) {
        fprintf(stderr, "transport_pty_init: session_name too long (%zu >= %zu)\n",
                name_len, sizeof(((pty_ctx_t*)0)->session_name));
        return -1;
    }

    /* Belt-and-suspenders: assert after the recoverable checks */
    ASSERT_MSG(path_len < sizeof(((pty_ctx_t*)0)->pty_path),
               "transport_pty_init: pty_path length check bypassed");
    ASSERT_MSG(name_len < sizeof(((pty_ctx_t*)0)->session_name),
               "transport_pty_init: session_name length check bypassed");

    pty_ctx_t *ctx = calloc(1, sizeof(pty_ctx_t));
    if (!ctx) return -1;

    snprintf(ctx->pty_path, sizeof(ctx->pty_path), "%s", pty_path);
    snprintf(ctx->session_name, sizeof(ctx->session_name), "%s", session_name);

    tp->capture = pty_capture;
    tp->send_text = pty_send_text;
    tp->send_key = pty_send_key;
    tp->is_alive = pty_is_alive;
    tp->ctx = ctx;

    /* Postcondition: all vtable entries set */
    ASSERT_MSG(tp->capture != NULL && tp->send_text != NULL &&
               tp->send_key != NULL && tp->is_alive != NULL && tp->ctx != NULL,
               "transport_pty_init: postcondition violated - NULL vtable entry");
    return 0;
}

void transport_free(transport_t *tp) {
    if (!tp) return;

    /* Violation 10 (HARDENING): verify transport is either fully initialised
     * or fully zeroed -- partial init indicates corruption */
    int all_null = (tp->capture == NULL && tp->send_text == NULL &&
                    tp->send_key == NULL && tp->is_alive == NULL);
    int all_set = (tp->capture != NULL && tp->send_text != NULL &&
                   tp->send_key != NULL && tp->is_alive != NULL);
    ASSERT_MSG(all_null || all_set,
               "transport_free: partially initialised transport "
               "(capture=%p, send_text=%p, send_key=%p, is_alive=%p) -- corrupt state",
               (void*)tp->capture, (void*)tp->send_text,
               (void*)tp->send_key, (void*)tp->is_alive);

    free(tp->ctx);
    memset(tp, 0, sizeof(*tp));
}
