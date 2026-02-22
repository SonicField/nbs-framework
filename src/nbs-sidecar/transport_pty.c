/*
 * transport_pty.c — pty-session transport implementation.
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

/* Context for pty-session transport */
typedef struct {
    char pty_path[4096];
    char session_name[256];
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

    char *buf = malloc(32768);
    if (!buf) return NULL;

    int rc = exec_capture(argv, buf, 32768);
    if (rc < 0) {
        free(buf);
        return NULL;
    }

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

    return exec_fire_and_forget(argv) == 0 ? 0 : -1;
}

static int pty_send_key(const transport_t *self, const char *key) {
    ASSERT_MSG(self != NULL, "pty_send_key: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "pty_send_key: ctx is NULL");
    ASSERT_MSG(key != NULL, "pty_send_key: key is NULL");
    const pty_ctx_t *ctx = self->ctx;

    if (strcmp(key, "Enter") == 0) {
        /* pty-session: empty string sends bare Enter */
        const char *argv[] = {
            ctx->pty_path, "send", ctx->session_name, "", NULL
        };
        return exec_fire_and_forget(argv) == 0 ? 0 : -1;
    } else if (strcmp(key, "Escape") == 0) {
        /* pty-session: send raw escape byte via --no-enter */
        const char *argv[] = {
            ctx->pty_path, "send", ctx->session_name, "--no-enter", "\x1b", NULL
        };
        return exec_fire_and_forget(argv) == 0 ? 0 : -1;
    } else {
        /* Unsupported key — try sending as text */
        const char *argv[] = {
            ctx->pty_path, "send", ctx->session_name, "--no-enter", key, NULL
        };
        return exec_fire_and_forget(argv) == 0 ? 0 : -1;
    }
}

static int pty_is_alive(const transport_t *self) {
    ASSERT_MSG(self != NULL, "pty_is_alive: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "pty_is_alive: ctx is NULL");
    const pty_ctx_t *ctx = self->ctx;

    char buf[4096];
    const char *argv[] = {
        ctx->pty_path, "list", NULL
    };

    int rc = exec_capture(argv, buf, sizeof(buf));
    if (rc != 0) return 0;

    /* Check if session_name appears as a complete line in the output */
    char *line = strtok(buf, "\n");
    while (line) {
        /* Trim leading/trailing whitespace */
        while (*line == ' ' || *line == '\t') line++;
        char *end = line + strlen(line) - 1;
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
    ASSERT_MSG(session_name[0] != '\0', "transport_pty_init: session_name is empty");

    memset(tp, 0, sizeof(*tp));

    pty_ctx_t *ctx = calloc(1, sizeof(pty_ctx_t));
    if (!ctx) return -1;

    ASSERT_MSG(strlen(pty_path) < sizeof(ctx->pty_path),
               "transport_pty_init: pty_path too long (%zu >= %zu)",
               strlen(pty_path), sizeof(ctx->pty_path));
    ASSERT_MSG(strlen(session_name) < sizeof(ctx->session_name),
               "transport_pty_init: session_name too long (%zu >= %zu)",
               strlen(session_name), sizeof(ctx->session_name));
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
    free(tp->ctx);
    memset(tp, 0, sizeof(*tp));
}
