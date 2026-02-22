/*
 * transport_tmux.c — tmux transport implementation.
 *
 * Implements the transport vtable for tmux sessions. Each operation
 * is a fork+exec of the tmux binary.
 */

#include "transport.h"
#include "exec_util.h"
#include "../nbs-common/nbs_assert.h"

#include <stdlib.h>
#include <string.h>

/* Context for tmux transport */
typedef struct {
    char pane_id[64];
} tmux_ctx_t;

static char *tmux_capture(const transport_t *self, int scrollback) {
    ASSERT_MSG(self != NULL, "tmux_capture: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "tmux_capture: ctx is NULL");
    const tmux_ctx_t *ctx = self->ctx;

    ASSERT_MSG(scrollback >= 0, "tmux_capture: scrollback must be non-negative, got %d", scrollback);
    char scroll_arg[32];
    snprintf(scroll_arg, sizeof(scroll_arg), "-%d", scrollback);

    const char *argv[] = {
        "tmux", "capture-pane", "-t", ctx->pane_id, "-p", "-S", scroll_arg, NULL
    };

    /* Allocate buffer for captured content */
    char *buf = malloc(32768);
    if (!buf) return NULL;

    int rc = exec_capture(argv, buf, 32768);
    if (rc < 0) {
        free(buf);
        return NULL;
    }

    return buf;
}

static int tmux_send_text(const transport_t *self, const char *text) {
    ASSERT_MSG(self != NULL, "tmux_send_text: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "tmux_send_text: ctx is NULL");
    ASSERT_MSG(text != NULL, "tmux_send_text: text is NULL");
    const tmux_ctx_t *ctx = self->ctx;

    const char *argv[] = {
        "tmux", "send-keys", "-t", ctx->pane_id, "-l", text, NULL
    };

    return exec_fire_and_forget(argv) == 0 ? 0 : -1;
}

static int tmux_send_key(const transport_t *self, const char *key) {
    ASSERT_MSG(self != NULL, "tmux_send_key: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "tmux_send_key: ctx is NULL");
    ASSERT_MSG(key != NULL, "tmux_send_key: key is NULL");
    const tmux_ctx_t *ctx = self->ctx;

    const char *argv[] = {
        "tmux", "send-keys", "-t", ctx->pane_id, key, NULL
    };

    return exec_fire_and_forget(argv) == 0 ? 0 : -1;
}

static int tmux_is_alive(const transport_t *self) {
    ASSERT_MSG(self != NULL, "tmux_is_alive: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "tmux_is_alive: ctx is NULL");
    const tmux_ctx_t *ctx = self->ctx;

    char buf[256];
    const char *argv[] = {
        "tmux", "list-panes", "-t", ctx->pane_id, NULL
    };

    int rc = exec_capture(argv, buf, sizeof(buf));
    return (rc == 0) ? 1 : 0;
}

int transport_tmux_init(transport_t *tp, const char *pane_id) {
    ASSERT_MSG(tp != NULL, "transport_tmux_init: tp is NULL");
    ASSERT_MSG(pane_id != NULL, "transport_tmux_init: pane_id is NULL");
    ASSERT_MSG(pane_id[0] != '\0', "transport_tmux_init: pane_id is empty");

    memset(tp, 0, sizeof(*tp));

    tmux_ctx_t *ctx = calloc(1, sizeof(tmux_ctx_t));
    if (!ctx) return -1;

    ASSERT_MSG(strlen(pane_id) < sizeof(ctx->pane_id),
               "transport_tmux_init: pane_id too long (%zu >= %zu)",
               strlen(pane_id), sizeof(ctx->pane_id));
    snprintf(ctx->pane_id, sizeof(ctx->pane_id), "%s", pane_id);

    tp->capture = tmux_capture;
    tp->send_text = tmux_send_text;
    tp->send_key = tmux_send_key;
    tp->is_alive = tmux_is_alive;
    tp->ctx = ctx;

    return 0;
}
