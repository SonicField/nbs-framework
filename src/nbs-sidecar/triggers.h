/*
 * triggers.h — Periodic trigger functions (pythia, standup, heartbeat).
 */

#ifndef NBS_TRIGGERS_H
#define NBS_TRIGGERS_H

#include <time.h>

/*
 * trigger_pythia_check — Deterministic Pythia checkpoint trigger.
 *
 * Counts decision-logged events in processed/. When count crosses
 * a multiple of pythia-interval (from config.yaml, default 20),
 * spawns Pythia worker (Phase 2) or publishes bus event (Phase 1).
 *
 * Returns: 0 = action taken, 1 = no action, -1 = error
 */
int trigger_pythia_check(const char *registry_path, const char *nbs_root,
                          int *last_trigger_count);

/*
 * trigger_standup_check — CSMA/CD team check-in via chat.
 *
 * Posts standup message to first registered chat after interval_minutes.
 * Uses shared timestamp file for collision avoidance with random backoff.
 *
 * Returns: 0 = posted, 1 = no action, -1 = error
 */
int trigger_standup_check(const char *registry_path, const char *nbs_root,
                           const char *handle, int interval_minutes,
                           time_t *last_standup_time);

/*
 * trigger_heartbeat — Active heartbeat post to chat.
 *
 * Posts "still active" message if interval seconds have elapsed.
 * interval=0 disables heartbeats.
 *
 * Returns: 0 = posted, 1 = no action
 */
int trigger_heartbeat(const char *registry_path, const char *handle,
                       int interval, time_t *last_heartbeat_time);

/*
 * trigger_pythia_spawn — Spawn Pythia worker via lock + fork+exec.
 *
 * Phase 2: replaces bus event publication with direct worker spawn.
 * Uses non-blocking lock to prevent concurrent spawns across sidecars.
 *
 * Returns: 0 = spawned, 1 = lock busy (another sidecar handling it), -1 = error
 */
int trigger_pythia_spawn(const char *nbs_root);

#endif /* NBS_TRIGGERS_H */
