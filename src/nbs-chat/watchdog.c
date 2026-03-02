/*
 * watchdog.c — Terminal watchdog daemon state machine
 *
 * Pure decision logic for detecting team death and rate-limiting
 * restarts. No threads, no I/O — all side effects are the caller's
 * responsibility.
 */

#include "watchdog.h"
#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <string.h>

void watchdog_init(watchdog_state_t *ws,
                   const char *chat_path,
                   const char *project_root) {
    ASSERT_MSG(ws != NULL, "watchdog_init: ws is NULL");
    ASSERT_MSG(chat_path != NULL && chat_path[0] != '\0',
               "watchdog_init: chat_path is NULL or empty");
    ASSERT_MSG(project_root != NULL && project_root[0] != '\0',
               "watchdog_init: project_root is NULL or empty");

    atomic_store(&ws->enabled, 1);
    ws->restart_count = 0;
    ws->window_start = 0;
    ws->last_restart = 0;

    int sn = snprintf(ws->chat_path, sizeof(ws->chat_path), "%s", chat_path);
    ASSERT_MSG(sn > 0 && (size_t)sn < sizeof(ws->chat_path),
               "watchdog_init: chat_path truncated");
    sn = snprintf(ws->project_root, sizeof(ws->project_root), "%s", project_root);
    ASSERT_MSG(sn > 0 && (size_t)sn < sizeof(ws->project_root),
               "watchdog_init: project_root truncated");
}

watchdog_decision_t watchdog_evaluate(watchdog_state_t *ws,
                                      int alive_count,
                                      time_t now) {
    ASSERT_MSG(ws != NULL, "watchdog_evaluate: ws is NULL");
    ASSERT_MSG(alive_count >= 0,
               "watchdog_evaluate: negative alive_count: %d", alive_count);
    ASSERT_MSG(now > 0, "watchdog_evaluate: invalid timestamp: %ld", (long)now);

    /* Check enabled flag (atomic — safe across threads) */
    if (!atomic_load(&ws->enabled)) {
        return WATCHDOG_DISABLED;
    }

    /* Team alive — reset rate counter if window has elapsed */
    if (alive_count >= WATCHDOG_MIN_ALIVE) {
        if (ws->window_start > 0 &&
            now - ws->window_start >= WATCHDOG_RATE_WINDOW_S) {
            ws->restart_count = 0;
            ws->window_start = now;
        }
        return WATCHDOG_NO_ACTION;
    }

    /* Team dead — check cooldown */
    if (ws->last_restart > 0 &&
        now - ws->last_restart < WATCHDOG_COOLDOWN_S) {
        return WATCHDOG_NO_ACTION;
    }

    /* Reset window if expired or not yet opened */
    if (ws->window_start == 0 ||
        now - ws->window_start >= WATCHDOG_RATE_WINDOW_S) {
        ws->restart_count = 0;
        ws->window_start = now;
    }

    /* Rate limit check */
    if (ws->restart_count >= WATCHDOG_MAX_RESTARTS) {
        atomic_store(&ws->enabled, 0);
        return WATCHDOG_RATE_LIMITED;
    }

    /* Trigger restart */
    ws->restart_count++;
    ws->last_restart = now;
    return WATCHDOG_RESTART;
}

void watchdog_disable(watchdog_state_t *ws) {
    ASSERT_MSG(ws != NULL, "watchdog_disable: ws is NULL");
    atomic_store(&ws->enabled, 0);
}

void watchdog_enable(watchdog_state_t *ws) {
    ASSERT_MSG(ws != NULL, "watchdog_enable: ws is NULL");
    atomic_store(&ws->enabled, 1);
}

int watchdog_is_enabled(const watchdog_state_t *ws) {
    ASSERT_MSG(ws != NULL, "watchdog_is_enabled: ws is NULL");
    return atomic_load(&ws->enabled);
}
