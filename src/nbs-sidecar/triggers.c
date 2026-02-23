/*
 * triggers.c — Periodic trigger functions (pythia, standup, heartbeat).
 *
 * These functions are called from the sidecar main loop to perform
 * time-based or count-based actions. Each trigger is independent
 * and has no side effects on the others.
 */

#include "triggers.h"
#include "bus_client.h"
#include "chat_client.h"
#include "exec_util.h"
#include "registry.h"
#include "../nbs-common/nbs_assert.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- Pythia trigger --- */

/*
 * count_decision_events — Count decision-logged event files in processed/.
 */
static int count_decision_events(const char *bus_dir) {
    ASSERT_MSG(strlen(bus_dir) < 4000,
           "count_decision_events: bus_dir too long: %zu", strlen(bus_dir));
    char processed_path[4096];
    int n = snprintf(processed_path, sizeof(processed_path),
                     "%s/processed", bus_dir);
    if (n < 0 || (size_t)n >= sizeof(processed_path)) return 0;

    DIR *d = opendir(processed_path);
    if (!d) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strstr(entry->d_name, "decision-logged") != NULL) {
            count++;
        }
    }
    int crc = closedir(d);
    ASSERT_MSG(crc == 0, "count_decision_events: closedir failed: %s", strerror(errno));
    return count;
}

/*
 * read_pythia_interval — Read pythia-interval from config.yaml.
 */
static int read_pythia_interval(const char *bus_dir) {
    ASSERT_MSG(strlen(bus_dir) < 4000,
           "read_pythia_interval: bus_dir too long: %zu", strlen(bus_dir));
    char config_path[4096];
    int n = snprintf(config_path, sizeof(config_path),
                     "%s/config.yaml", bus_dir);
    if (n < 0 || (size_t)n >= sizeof(config_path)) return 20;

    FILE *f = fopen(config_path, "r");
    if (!f) return 20;

    int interval = 20;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "pythia-interval:", 16) == 0) {
            char *val = line + 16;
            while (*val == ' ' || *val == '\t') val++;
            char *endptr;
            long parsed = strtol(val, &endptr, 10);
            if (endptr != val && (*endptr == '\n' || *endptr == '\0') && parsed > 0 && parsed <= 100000)
                interval = (int)parsed;
            break;
        }
    }
    fclose(f);
    return interval;
}

int trigger_pythia_check(const char *registry_path, const char *nbs_root,
                          int *last_trigger_count) {
    ASSERT_MSG(registry_path != NULL, "trigger_pythia_check: registry_path is NULL");
    ASSERT_MSG(nbs_root != NULL, "trigger_pythia_check: nbs_root is NULL");
    ASSERT_MSG(last_trigger_count != NULL, "trigger_pythia_check: last_trigger_count is NULL");

    /* Find bus directory from registry */
    char bus_dir[4096];
    if (registry_find_first(registry_path, "bus", bus_dir, sizeof(bus_dir)) != 0) {
        return 1; /* No bus registered */
    }

    int interval = read_pythia_interval(bus_dir);
    int decision_count = count_decision_events(bus_dir);

    /* Check if we've crossed a new threshold */
    int current_bucket = decision_count / interval;
    int last_bucket = *last_trigger_count / interval;

    if (current_bucket > last_bucket && decision_count > 0) {
        *last_trigger_count = decision_count;

        /* Publish bus event for observability */
        char payload[256];
        snprintf(payload, sizeof(payload),
                 "Decision count: %d. Sidecar-triggered Pythia assessment.",
                 decision_count);
        bus_client_publish(bus_dir, "sidecar", "pythia-checkpoint", "high",
                           payload);

        /* Spawn Pythia worker (lock-guarded, fire-and-forget) */
        trigger_pythia_spawn(nbs_root);
        return 0;
    }

    /* Sync counter on first run (catch-up without firing) */
    if (*last_trigger_count == 0 && decision_count > 0) {
        *last_trigger_count = decision_count;
    }

    return 1;
}

/* --- Standup trigger --- */

/*
 * CSMA/CD protocol for standup coordination.
 *
 * Multiple sidecars may independently decide to post a standup. To avoid
 * duplicate posts, we use a shared timestamp file and random backoff —
 * the same principle as Ethernet CSMA/CD.
 *
 * This is probabilistic by design, not deterministic. A lock-based approach
 * would be brittle: if a sidecar crashes while holding the lock, all others
 * stall until the lock times out or is manually cleared. The probabilistic
 * approach degrades gracefully — worst case is a rare duplicate standup,
 * which is harmless. Best case is exactly one post per interval.
 *
 * The Pythia trigger (trigger_pythia_spawn) uses lock files because Pythia
 * spawning is expensive and must not duplicate. If lock-based coordination
 * proves more reliable over the long term, standup could be migrated to
 * the same pattern. For now, the evidence favours probabilistic: simpler,
 * no failure modes, acceptable duplicate rate.
 */

int trigger_standup_check(const char *registry_path, const char *nbs_root,
                           const char *handle, int interval_minutes,
                           time_t *last_standup_time) {
    ASSERT_MSG(registry_path != NULL, "trigger_standup_check: registry_path is NULL");
    ASSERT_MSG(nbs_root != NULL, "trigger_standup_check: nbs_root is NULL");
    ASSERT_MSG(handle != NULL, "trigger_standup_check: handle is NULL");
    ASSERT_MSG(last_standup_time != NULL, "trigger_standup_check: last_standup_time is NULL");

    if (interval_minutes <= 0) return 1;

    time_t now = time(NULL);
    int interval_secs = interval_minutes * 60;

    /* Skip if not enough time since our last attempt */
    if (*last_standup_time > 0 && (now - *last_standup_time) < interval_secs) {
        return 1;
    }

    /* First run: initialise timer without posting */
    if (*last_standup_time == 0) {
        *last_standup_time = now;
        return 1;
    }

    /* Find first chat */
    char chat_path[4096];
    if (registry_find_first(registry_path, "chat", chat_path, sizeof(chat_path)) != 0) {
        return 1;
    }

    /* CSMA/CD: check shared timestamp file */
    char ts_file[4096 + 16];
    snprintf(ts_file, sizeof(ts_file), "%s.standup-ts", chat_path);

    time_t last_global = 0;
    FILE *tsf = fopen(ts_file, "r");
    if (tsf) {
        char buf[32];
        if (fgets(buf, sizeof(buf), tsf)) {
            char *endptr;
            long parsed_ts = strtol(buf, &endptr, 10);
            if (endptr != buf && parsed_ts >= 0)
                last_global = (time_t)parsed_ts;
        }
        fclose(tsf);
    }

    if (last_global > 0 && (now - last_global) < interval_secs) {
        /* Medium is busy — random backoff */
        unsigned int rand_val;
        int ufd = open("/dev/urandom", O_RDONLY);
        if (ufd >= 0) {
            if (read(ufd, &rand_val, sizeof(rand_val)) < 0) {
                rand_val = (unsigned int)now;
            }
            close(ufd);
        } else {
            rand_val = (unsigned int)now;
        }
        int backoff = (int)(rand_val % ((unsigned int)(interval_secs / 2) + 1));
        *last_standup_time = now - interval_secs + backoff;
        return 1;
    }

    /* Post standup */
    const char *standup_msg =
        "Check-in: @scribe post a summary of decisions and open items "
        "since the last check-in. @supervisor once scribe has posted, "
        "review and assign next tasks. All agents: what are you working on? "
        "What is blocked? If you are idle, find useful work NOW \xe2\x80\x94 "
        "do not wait for assignment. If you declared session-end without "
        "supervisor approval, resume work immediately.";

    chat_client_send(chat_path, "sidecar", standup_msg);

    /* Update shared timestamp atomically */
    char ts_tmp[4096 + 24];
    snprintf(ts_tmp, sizeof(ts_tmp), "%s.tmp", ts_file);
    FILE *wtf = fopen(ts_tmp, "w");
    if (wtf) {
        fprintf(wtf, "%ld\n", (long)now);
        if (fclose(wtf) == 0) {
            rename(ts_tmp, ts_file);
        } else {
            unlink(ts_tmp);
        }
    }

    *last_standup_time = now;
    return 0;
}

/* --- Heartbeat trigger --- */

int trigger_heartbeat(const char *registry_path, const char *handle,
                       int interval, time_t *last_heartbeat_time) {
    ASSERT_MSG(registry_path != NULL, "trigger_heartbeat: registry_path is NULL");
    ASSERT_MSG(handle != NULL, "trigger_heartbeat: handle is NULL");
    ASSERT_MSG(last_heartbeat_time != NULL, "trigger_heartbeat: last_heartbeat_time is NULL");

    if (interval <= 0) return 1;

    time_t now = time(NULL);

    if (*last_heartbeat_time == 0) {
        *last_heartbeat_time = now;
        return 1;
    }

    if ((now - *last_heartbeat_time) < interval) {
        return 1;
    }

    char chat_path[4096];
    if (registry_find_first(registry_path, "chat", chat_path, sizeof(chat_path)) != 0) {
        return 1;
    }

    long elapsed = (long)(now - *last_heartbeat_time);
    char msg[512];
    snprintf(msg, sizeof(msg),
             "Heartbeat: %s is actively processing (%lds since last heartbeat). "
             "Pane content is changing \xe2\x80\x94 not idle.",
             handle, elapsed);

    chat_client_send(chat_path, "sidecar", msg);
    *last_heartbeat_time = now;
    return 0;
}

/* --- Phase 2: Pythia spawn --- */

int trigger_pythia_spawn(const char *nbs_root) {
    ASSERT_MSG(nbs_root != NULL, "trigger_pythia_spawn: nbs_root is NULL");

    /* Build lock path */
    char lock_path[4096];
    int n = snprintf(lock_path, sizeof(lock_path),
                     "%s/.nbs/pythia.lock", nbs_root);
    ASSERT_MSG(n > 0 && (size_t)n < sizeof(lock_path),
               "trigger_pythia_spawn: lock path overflow");

    /* Non-blocking lock acquisition */
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "trigger_pythia_spawn: open lock failed: %s\n",
                strerror(errno));
        return -1;
    }

    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };

    /* F_SETLK (non-blocking) — if can't acquire, another sidecar won */
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        close(fd);
        return 1; /* Lock busy */
    }

    /* Build task description */
    const char *task_desc =
        "Load /nbs-pythia. Read .nbs/scribe/live-log.md. "
        "Run the checkpoint procedure. Post assessment to chat. Exit.";

    /* Fork+exec nbs-worker spawn */
    const char *argv[] = {
        "nbs-worker", "spawn", "pythia", nbs_root, task_desc, NULL
    };
    int rc = exec_fire_and_forget(argv);

    /* Release lock */
    struct flock unlock = {
        .l_type = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    fcntl(fd, F_SETLK, &unlock);
    close(fd);

    return (rc == 0) ? 0 : -1;
}
