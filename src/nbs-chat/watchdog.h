/*
 * watchdog.h — Project state for nbs-chat-terminal
 *
 * Holds the project root and chat path resolved at startup,
 * plus an enabled/disabled flag for /pause and /resume.
 *
 * Used by oracle commands (/pythia, /shepard, etc.), /pause,
 * /resume, /kick, /health, /restart, and auto-repair.
 *
 * Usage:
 *   watchdog_state_t ws;
 *   watchdog_init(&ws, ".nbs/chat/live.chat", "/home/user/project");
 *   watchdog_disable(&ws);  // on /pause or /shutdown
 *   watchdog_enable(&ws);   // on /resume
 */

#ifndef NBS_WATCHDOG_H
#define NBS_WATCHDOG_H

#include <stdatomic.h>

/*
 * Terminal project state.
 *
 * 'enabled' is accessed from /pause and /resume (main thread only
 * now that the daemon thread is removed, but kept atomic for safety).
 *
 * 'chat_path' and 'project_root' are read-only after init.
 */
typedef struct {
    atomic_int enabled;
    char chat_path[4096];
    char project_root[4096];
} watchdog_state_t;

/*
 * Initialise state. Copies paths.
 *
 * Preconditions:
 *   ws != NULL
 *   chat_path != NULL, non-empty
 *   project_root != NULL, non-empty
 *
 * Postcondition: enabled = 1.
 */
void watchdog_init(watchdog_state_t *ws,
                   const char *chat_path,
                   const char *project_root);

/* Disable (pause). */
void watchdog_disable(watchdog_state_t *ws);

/* Enable (resume). */
void watchdog_enable(watchdog_state_t *ws);

/* Check if enabled. */
int watchdog_is_enabled(const watchdog_state_t *ws);

#endif /* NBS_WATCHDOG_H */
