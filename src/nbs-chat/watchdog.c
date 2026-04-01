/*
 * watchdog.c — Project state for nbs-chat-terminal
 *
 * Holds project root, chat path, and enabled/disabled flag.
 * No threads, no I/O, no auto-restart logic.
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

    int sn = snprintf(ws->chat_path, sizeof(ws->chat_path), "%s", chat_path);
    ASSERT_MSG(sn > 0 && (size_t)sn < sizeof(ws->chat_path),
               "watchdog_init: chat_path truncated");
    sn = snprintf(ws->project_root, sizeof(ws->project_root), "%s", project_root);
    ASSERT_MSG(sn > 0 && (size_t)sn < sizeof(ws->project_root),
               "watchdog_init: project_root truncated");

    ASSERT_MSG(atomic_load(&ws->enabled) == 1,
               "watchdog_init postcondition: enabled must be 1, got %d",
               atomic_load(&ws->enabled));
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
