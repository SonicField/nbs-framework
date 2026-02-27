/*
 * transport_tmux.c -- tmux transport implementation.
 *
 * Implements the transport vtable for tmux sessions. Each operation
 * is a fork+exec of the tmux binary.
 */

#include "transport.h"
#include "exec_util.h"
#include "../nbs-common/nbs_assert.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Named constant for capture buffer size (Violation 1: HARDENING) */
#define TMUX_CAPTURE_BUF_SIZE 32768

/* Context for tmux transport */
typedef struct {
    char pane_id[64];
} tmux_ctx_t;

static char *tmux_capture(const transport_t *self, int scrollback) {
    ASSERT_MSG(self != NULL, "tmux_capture: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "tmux_capture: ctx is NULL");
    const tmux_ctx_t *ctx = self->ctx;

    /* Violation 3 (BUG): scrollback==0 produces "-0" which tmux interprets
     * as "start of history", capturing the entire scrollback buffer.
     * scrollback must be strictly positive for -S argument. */
    ASSERT_MSG(scrollback >= 0, "tmux_capture: scrollback must be non-negative, got %d", scrollback);

    const char *argv_with_scroll[] = {
        "tmux", "capture-pane", "-t", ctx->pane_id, "-p", "-S", NULL, NULL
    };
    const char *argv_no_scroll[] = {
        "tmux", "capture-pane", "-t", ctx->pane_id, "-p", NULL
    };

    char scroll_arg[32];
    const char *const *argv;
    if (scrollback > 0) {
        snprintf(scroll_arg, sizeof(scroll_arg), "-%d", scrollback);
        /* Build argv with the scroll argument */
        argv_with_scroll[6] = scroll_arg;
        argv = argv_with_scroll;
    } else {
        /* scrollback == 0: capture only the visible pane (no -S flag) */
        argv = argv_no_scroll;
    }

    /* Allocate buffer for captured content */
    char *buf = malloc(TMUX_CAPTURE_BUF_SIZE);
    if (!buf) return NULL;

    int rc = exec_capture(argv, buf, TMUX_CAPTURE_BUF_SIZE);
    if (rc < 0) {
        fprintf(stderr, "tmux_capture: exec_capture failed (rc=%d) for pane '%s'\n",
                rc, ctx->pane_id);
        free(buf);
        return NULL;
    }

    /* Violation 4 (HARDENING): postcondition -- buffer is NUL-terminated */
    ASSERT_MSG(buf[strlen(buf)] == '\0',
               "tmux_capture: buffer not NUL-terminated after exec_capture");
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

    /* Violation 6 (HARDENING): log exec failure */
    int rc = exec_fire_and_forget(argv);
    if (rc != 0) {
        fprintf(stderr, "tmux_send_text: exec failed (rc=%d) for pane '%s'\n",
                rc, ctx->pane_id);
        return -1;
    }
    return 0;
}

static int tmux_send_key(const transport_t *self, const char *key) {
    ASSERT_MSG(self != NULL, "tmux_send_key: self is NULL");
    ASSERT_MSG(self->ctx != NULL, "tmux_send_key: ctx is NULL");
    ASSERT_MSG(key != NULL, "tmux_send_key: key is NULL");
    const tmux_ctx_t *ctx = self->ctx;

    const char *argv[] = {
        "tmux", "send-keys", "-t", ctx->pane_id, key, NULL
    };

    /* Violation 6 (HARDENING): log exec failure */
    int rc = exec_fire_and_forget(argv);
    if (rc != 0) {
        fprintf(stderr, "tmux_send_key: exec failed (rc=%d) for pane '%s', key '%s'\n",
                rc, ctx->pane_id, key);
        return -1;
    }
    return 0;
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
    /* Violation 8 (BUG): distinguish exec error from pane-not-found */
    if (rc < 0) {
        fprintf(stderr, "tmux_is_alive: exec_capture failed (rc=%d) for pane '%s'\n",
                rc, ctx->pane_id);
        return -1;  /* exec error */
    }
    return (rc == 0) ? 1 : 0;  /* pane exists or not */
}

int transport_tmux_init(transport_t *tp, const char *pane_id) {
    ASSERT_MSG(tp != NULL, "transport_tmux_init: tp is NULL");
    ASSERT_MSG(pane_id != NULL, "transport_tmux_init: pane_id is NULL");

    /* Violation 5 (BUG): memset before any checks so error paths
     * honour the postcondition "tp is zeroed on error" */
    memset(tp, 0, sizeof(*tp));

    /* Violation 2 (BUG): empty/too-long pane_id returns -1, not abort.
     * pane_id originates from user/environment input. */
    if (pane_id[0] == '\0') {
        fprintf(stderr, "transport_tmux_init: pane_id is empty\n");
        return -1;
    }

    size_t pane_id_len = strlen(pane_id);
    if (pane_id_len >= sizeof(((tmux_ctx_t*)0)->pane_id)) {
        fprintf(stderr, "transport_tmux_init: pane_id too long (%zu >= %zu)\n",
                pane_id_len, sizeof(((tmux_ctx_t*)0)->pane_id));
        return -1;
    }

    /* Violation 7 (SECURITY): validate pane_id matches tmux format %[0-9]+.
     * Prevents arbitrary strings being passed to tmux. */
    if (pane_id[0] != '%') {
        fprintf(stderr, "transport_tmux_init: pane_id must start with '%%', got '%s'\n",
                pane_id);
        return -1;
    }
    if (pane_id_len < 2) {
        fprintf(stderr, "transport_tmux_init: pane_id '%%' has no digits\n");
        return -1;
    }
    for (size_t i = 1; pane_id[i]; i++) {
        if (pane_id[i] < '0' || pane_id[i] > '9') {
            fprintf(stderr, "transport_tmux_init: pane_id contains non-digit at position %zu: '%s'\n",
                    i, pane_id);
            return -1;
        }
    }

    /* Belt-and-suspenders assert after recoverable check */
    ASSERT_MSG(pane_id_len < sizeof(((tmux_ctx_t*)0)->pane_id),
               "transport_tmux_init: pane_id length check bypassed");

    tmux_ctx_t *ctx = calloc(1, sizeof(tmux_ctx_t));
    if (!ctx) return -1;

    snprintf(ctx->pane_id, sizeof(ctx->pane_id), "%s", pane_id);

    tp->capture = tmux_capture;
    tp->send_text = tmux_send_text;
    tp->send_key = tmux_send_key;
    tp->is_alive = tmux_is_alive;
    tp->ctx = ctx;

    /* Postcondition: all vtable entries set */
    ASSERT_MSG(tp->capture != NULL && tp->send_text != NULL &&
               tp->send_key != NULL && tp->is_alive != NULL && tp->ctx != NULL,
               "transport_tmux_init: postcondition violated - NULL vtable entry");
    return 0;
}
