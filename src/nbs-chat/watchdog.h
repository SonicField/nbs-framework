/*
 * watchdog.h — Terminal watchdog daemon state machine
 *
 * Detects when the NBS agent team has died (<3 tmux sessions alive)
 * and decides whether to trigger an auto-restart. Pure state machine
 * with no I/O or threading — all I/O is done by the caller.
 *
 * The only cross-thread field is 'enabled' (atomic_int). All other
 * fields are owned by the daemon thread after initialisation.
 *
 * Usage:
 *   watchdog_state_t ws;
 *   watchdog_init(&ws, ".nbs/chat/live.chat", "/home/user/project");
 *   // In daemon thread:
 *   watchdog_decision_t d = watchdog_evaluate(&ws, alive_count, time(NULL));
 *   if (d == WATCHDOG_RESTART) { system("bash restart.sh ..."); }
 *   // From main thread:
 *   watchdog_disable(&ws);  // on /shutdown
 */

#ifndef NBS_WATCHDOG_H
#define NBS_WATCHDOG_H

#include <stdatomic.h>
#include <time.h>

/* How often the daemon thread polls (seconds) */
#define WATCHDOG_POLL_INTERVAL_S     60

/* Minimum alive agent sessions before restart triggers */
#define WATCHDOG_MIN_ALIVE           3

/* Maximum auto-restarts per rolling hour */
#define WATCHDOG_MAX_RESTARTS        5

/* Rolling window for rate limiting (seconds) */
#define WATCHDOG_RATE_WINDOW_S       3600

/* Cooldown after a restart before allowing another (seconds) */
#define WATCHDOG_COOLDOWN_S          120

/* Restart decision result */
typedef enum {
    WATCHDOG_NO_ACTION,      /* Team alive or cooldown active */
    WATCHDOG_RESTART,        /* Trigger restart */
    WATCHDOG_RATE_LIMITED,   /* Hit rate limit — watchdog disabled itself */
    WATCHDOG_DISABLED        /* Watchdog is disabled */
} watchdog_decision_t;

/*
 * Watchdog state.
 *
 * 'enabled' is the ONLY field accessed from two threads:
 *   - main thread sets it to 0 on /shutdown
 *   - daemon thread reads it each poll cycle
 * All other fields are owned by the daemon thread after init.
 *
 * 'chat_path' and 'project_root' are read-only after init.
 */
typedef struct {
    atomic_int enabled;
    int restart_count;
    time_t window_start;
    time_t last_restart;
    char chat_path[4096];
    char project_root[4096];
} watchdog_state_t;

/*
 * Initialise watchdog state. Copies paths.
 *
 * Preconditions:
 *   ws != NULL
 *   chat_path != NULL, non-empty
 *   project_root != NULL, non-empty
 *
 * Postcondition: watchdog is enabled, restart_count = 0.
 */
void watchdog_init(watchdog_state_t *ws,
                   const char *chat_path,
                   const char *project_root);

/*
 * Evaluate whether to restart given current alive count and time.
 *
 * Deterministic function of (state, alive_count, now). Mutates ws.
 * Does NOT perform I/O.
 *
 * Preconditions:
 *   ws != NULL
 *   alive_count >= 0
 *   now > 0
 *
 * Returns:
 *   WATCHDOG_NO_ACTION    — team alive, or cooldown active
 *   WATCHDOG_RESTART      — trigger restart (restart_count incremented,
 *                           last_restart set to now)
 *   WATCHDOG_RATE_LIMITED  — hit max restarts this hour (enabled set to 0)
 *   WATCHDOG_DISABLED      — watchdog is disabled
 */
watchdog_decision_t watchdog_evaluate(watchdog_state_t *ws,
                                      int alive_count,
                                      time_t now);

/* Disable watchdog. Safe to call from any thread. */
void watchdog_disable(watchdog_state_t *ws);

/* Enable watchdog. Does NOT reset rate state. */
void watchdog_enable(watchdog_state_t *ws);

/* Check if watchdog is enabled. Safe to call from any thread. */
int watchdog_is_enabled(const watchdog_state_t *ws);

#endif /* NBS_WATCHDOG_H */
