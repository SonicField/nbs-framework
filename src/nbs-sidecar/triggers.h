/*
 * triggers.h — Periodic trigger functions for wall-clock worker spawning.
 *
 * Four triggers share a common pattern: shared timestamp file for
 * cross-sidecar dedup, lock-guarded fork+exec via nbs-workers.
 * Each trigger is defined by a static trigger_periodic_t config.
 */

#ifndef NBS_TRIGGERS_H
#define NBS_TRIGGERS_H

#include <time.h>

/*
 * trigger_periodic_t — Configuration for a periodic trigger.
 *
 * All fields are string literals (static lifetime). The trigger
 * system does not own or free any of these pointers.
 */
typedef struct {
    const char *name;           /* Human-readable name (e.g. "pythia") */
    const char *ts_filename;    /* Timestamp file under .nbs/ (e.g. "pythia-last-run") */
    const char *lock_filename;  /* Lock file under .nbs/ (e.g. "pythia.lock") */
    const char *role;           /* Worker role for nbs-workers spawn */
    const char *skill_file;     /* Skill file relative to .nbs/ (e.g. "commands/nbs-pythia.md") */
    const char *task_desc;      /* Task instructions appended after skill content */
    int first_delay_secs;       /* Delay before first fire (0 = use full interval) */
} trigger_periodic_t;

/* Static trigger definitions */
extern const trigger_periodic_t TRIGGER_PYTHIA;
extern const trigger_periodic_t TRIGGER_SHEPARD;
extern const trigger_periodic_t TRIGGER_FIXUP;
extern const trigger_periodic_t TRIGGER_LIBRARIAN;

/*
 * trigger_periodic_check — Wall-clock periodic trigger.
 *
 * Reads shared timestamp file. If interval_secs have elapsed since
 * last run, spawns the worker via trigger_periodic_spawn.
 * Cross-sidecar dedup via timestamp file + lock-guarded spawn.
 *
 * Returns: 0 = spawned, 1 = not time yet, -1 = error
 */
int trigger_periodic_check(const char *nbs_root, int interval_secs,
                           const trigger_periodic_t *trigger);

/*
 * trigger_periodic_spawn — Spawn worker via lock + fork+exec.
 *
 * Acquires a non-blocking fcntl write lock. If acquired, fork+execs
 * nbs-workers with the trigger's role and task description. Lock is
 * released after spawn command exits (not after worker completes).
 *
 * Returns: 0 = spawned, 1 = lock busy (another sidecar handling it), -1 = error
 */
int trigger_periodic_spawn(const char *nbs_root,
                           const trigger_periodic_t *trigger);

#endif /* NBS_TRIGGERS_H */
