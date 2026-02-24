/*
 * triggers.c — Periodic trigger functions (pythia, standup, heartbeat).
 *
 * These functions are called from the sidecar main loop to perform
 * time-based or count-based actions. Each trigger is independent
 * and has no side effects on the others.
 *
 * Standup trigger calls bin/nbs-prompts to select a randomised,
 * multilingual check-in prompt. Falls back to hardcoded English
 * if the script is unavailable.
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

/*
 * Cached absolute path to the nbs-workers binary.
 * Resolved once via /proc/self/exe — sibling binary in same directory.
 */
#define WORKERS_PATH_LEN 4096
static char nbs_workers_path[WORKERS_PATH_LEN] = "";
static int nbs_workers_path_resolved = 0;

static const char *resolve_nbs_workers(void)
{
    if (nbs_workers_path_resolved)
        return nbs_workers_path[0] ? nbs_workers_path : "nbs-workers";

    nbs_workers_path_resolved = 1;

    char self[WORKERS_PATH_LEN];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len <= 0)
        return "nbs-workers";
    self[len] = '\0';

    char *slash = strrchr(self, '/');
    if (!slash)
        return "nbs-workers";

    size_t dir_len = (size_t)(slash - self);
    if (dir_len + sizeof("/nbs-workers") > sizeof(nbs_workers_path))
        return "nbs-workers";

    memcpy(nbs_workers_path, self, dir_len);
    memcpy(nbs_workers_path + dir_len, "/nbs-workers", sizeof("/nbs-workers"));

    if (access(nbs_workers_path, X_OK) != 0) {
        nbs_workers_path[0] = '\0';
        return "nbs-workers";
    }

    return nbs_workers_path;
}

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

/*
 * read_shared_pythia_bucket — Read last triggered bucket from shared file.
 *
 * Multiple sidecars run independently, each with their own in-memory
 * last_trigger_count. Without coordination, all sidecars independently
 * detect a bucket transition and fire — producing duplicate Pythia
 * assessments. This shared file records which bucket was last triggered
 * by ANY sidecar, preventing duplicates.
 *
 * Returns the last triggered bucket number, or -1 if the file does
 * not exist or is unreadable (first run).
 */
static int read_shared_pythia_bucket(const char *nbs_root) {
    char path[4096];
    int n = snprintf(path, sizeof(path),
                     "%s/.nbs/pythia-last-bucket", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char buf[32];
    int bucket = -1;
    if (fgets(buf, sizeof(buf), f)) {
        char *endptr;
        long parsed = strtol(buf, &endptr, 10);
        if (endptr != buf && parsed >= 0)
            bucket = (int)parsed;
    }
    fclose(f);
    return bucket;
}

/*
 * write_shared_pythia_bucket — Atomically write last triggered bucket.
 *
 * Uses tmp+rename for atomicity — same pattern as standup timestamp.
 */
static void write_shared_pythia_bucket(const char *nbs_root, int bucket) {
    char path[4096], tmp_path[4096];
    int n = snprintf(path, sizeof(path),
                     "%s/.nbs/pythia-last-bucket", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    n = snprintf(tmp_path, sizeof(tmp_path),
                 "%s/.nbs/pythia-last-bucket.tmp", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) return;

    FILE *f = fopen(tmp_path, "w");
    if (f) {
        fprintf(f, "%d\n", bucket);
        if (fclose(f) == 0) {
            rename(tmp_path, path);
        } else {
            unlink(tmp_path);
        }
    }
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
        /*
         * Cross-sidecar dedup: check shared file before triggering.
         *
         * Multiple sidecars each have independent in-memory state. When
         * a bucket boundary is crossed, ALL sidecars detect it independently.
         * The shared file records which bucket was last triggered by ANY
         * sidecar. If another sidecar already triggered this bucket, skip.
         *
         * This is analogous to the standup CSMA/CD shared timestamp file
         * (.standup-ts), but uses bucket numbers instead of timestamps.
         * The flock in trigger_pythia_spawn provides a secondary guard,
         * but releases too quickly to prevent sequential duplicates.
         */
        int shared_bucket = read_shared_pythia_bucket(nbs_root);
        if (shared_bucket >= current_bucket) {
            /* Another sidecar already triggered this bucket */
            *last_trigger_count = decision_count;
            return 1;
        }

        /* We are the first sidecar to see this bucket — claim it */
        write_shared_pythia_bucket(nbs_root, current_bucket);
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

    /* Post standup — use randomised multilingual prompt if available */
    char prompt_buf[2048];
    const char *standup_msg;

    const char *argv[] = {"nbs-prompts", NULL};
    int rc = exec_capture(argv, prompt_buf, sizeof(prompt_buf));

    if (rc == 0 && prompt_buf[0] != '\0') {
        /* Strip trailing newline from script output */
        size_t len = strlen(prompt_buf);
        while (len > 0 && (prompt_buf[len - 1] == '\n' ||
                           prompt_buf[len - 1] == '\r'))
            prompt_buf[--len] = '\0';
        standup_msg = prompt_buf;
    } else {
        /* Fallback: hardcoded English prompt */
        standup_msg =
            "Check-in: @scribe post a summary of decisions and open items "
            "since the last check-in. @supervisor once scribe has posted, "
            "review and assign next tasks. All agents: what are you working "
            "on? What is blocked? If you are idle, find useful work NOW "
            "\xe2\x80\x94 do not wait for assignment. If you declared "
            "session-end without supervisor approval, resume work "
            "immediately.";
    }

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

    /* Fork+exec nbs-workers spawn */
    const char *argv[] = {
        resolve_nbs_workers(), "spawn", "pythia", nbs_root, task_desc, NULL
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

/* --- Shepard trigger --- */

/*
 * count_chat_message_events — Count chat-message event files in processed/.
 */
static int count_chat_message_events(const char *bus_dir) {
    ASSERT_MSG(strlen(bus_dir) < 4000,
           "count_chat_message_events: bus_dir too long: %zu", strlen(bus_dir));
    char processed_path[4096];
    int n = snprintf(processed_path, sizeof(processed_path),
                     "%s/processed", bus_dir);
    if (n < 0 || (size_t)n >= sizeof(processed_path)) return 0;

    DIR *d = opendir(processed_path);
    if (!d) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strstr(entry->d_name, "chat-message") != NULL) {
            count++;
        }
    }
    int crc = closedir(d);
    ASSERT_MSG(crc == 0, "count_chat_message_events: closedir failed: %s", strerror(errno));
    return count;
}

/*
 * read_shepard_interval — Read shepard-interval from config.yaml.
 */
static int read_shepard_interval(const char *bus_dir) {
    ASSERT_MSG(strlen(bus_dir) < 4000,
           "read_shepard_interval: bus_dir too long: %zu", strlen(bus_dir));
    char config_path[4096];
    int n = snprintf(config_path, sizeof(config_path),
                     "%s/config.yaml", bus_dir);
    if (n < 0 || (size_t)n >= sizeof(config_path)) return 100;

    FILE *f = fopen(config_path, "r");
    if (!f) return 100;

    int interval = 100;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "shepard-interval:", 17) == 0) {
            char *val = line + 17;
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

static int read_shared_shepard_bucket(const char *nbs_root) {
    char path[4096];
    int n = snprintf(path, sizeof(path),
                     "%s/.nbs/shepard-last-bucket", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char buf[32];
    int bucket = -1;
    if (fgets(buf, sizeof(buf), f)) {
        char *endptr;
        long parsed = strtol(buf, &endptr, 10);
        if (endptr != buf && parsed >= 0)
            bucket = (int)parsed;
    }
    fclose(f);
    return bucket;
}

static void write_shared_shepard_bucket(const char *nbs_root, int bucket) {
    char path[4096], tmp_path[4096];
    int n = snprintf(path, sizeof(path),
                     "%s/.nbs/shepard-last-bucket", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    n = snprintf(tmp_path, sizeof(tmp_path),
                 "%s/.nbs/shepard-last-bucket.tmp", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) return;

    FILE *f = fopen(tmp_path, "w");
    if (f) {
        fprintf(f, "%d\n", bucket);
        if (fclose(f) == 0) {
            rename(tmp_path, path);
        } else {
            unlink(tmp_path);
        }
    }
}

int trigger_shepard_check(const char *registry_path, const char *nbs_root,
                           int *last_trigger_count) {
    ASSERT_MSG(registry_path != NULL, "trigger_shepard_check: registry_path is NULL");
    ASSERT_MSG(nbs_root != NULL, "trigger_shepard_check: nbs_root is NULL");
    ASSERT_MSG(last_trigger_count != NULL, "trigger_shepard_check: last_trigger_count is NULL");

    char bus_dir[4096];
    if (registry_find_first(registry_path, "bus", bus_dir, sizeof(bus_dir)) != 0) {
        return 1;
    }

    int interval = read_shepard_interval(bus_dir);
    int message_count = count_chat_message_events(bus_dir);

    int current_bucket = message_count / interval;
    int last_bucket = *last_trigger_count / interval;

    if (current_bucket > last_bucket && message_count > 0) {
        /* Cross-sidecar dedup */
        int shared_bucket = read_shared_shepard_bucket(nbs_root);
        if (shared_bucket >= current_bucket) {
            *last_trigger_count = message_count;
            return 1;
        }

        write_shared_shepard_bucket(nbs_root, current_bucket);
        *last_trigger_count = message_count;

        /* Publish bus event */
        char payload[256];
        snprintf(payload, sizeof(payload),
                 "Message count: %d. Sidecar-triggered Shepard assessment.",
                 message_count);
        bus_client_publish(bus_dir, "sidecar", "shepard-checkpoint", "normal",
                           payload);

        trigger_shepard_spawn(nbs_root);
        return 0;
    }

    /* Sync counter on first run */
    if (*last_trigger_count == 0 && message_count > 0) {
        *last_trigger_count = message_count;
    }

    return 1;
}

int trigger_shepard_spawn(const char *nbs_root) {
    ASSERT_MSG(nbs_root != NULL, "trigger_shepard_spawn: nbs_root is NULL");

    char lock_path[4096];
    int n = snprintf(lock_path, sizeof(lock_path),
                     "%s/.nbs/shepard.lock", nbs_root);
    ASSERT_MSG(n > 0 && (size_t)n < sizeof(lock_path),
               "trigger_shepard_spawn: lock path overflow");

    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "trigger_shepard_spawn: open lock failed: %s\n",
                strerror(errno));
        return -1;
    }

    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };

    if (fcntl(fd, F_SETLK, &fl) < 0) {
        close(fd);
        return 1;
    }

    const char *task_desc =
        "Load /nbs-shepard. Check agent liveness via nbs-workers list and "
        "nbs-workers status — classify each as healthy/stressed/zombie/dead. "
        "Read recent chat via sub-agents. Assess team effectiveness. "
        "Post recommendations to supervisor (agent status FIRST). Exit.";

    const char *argv[] = {
        resolve_nbs_workers(), "spawn", "shepard", nbs_root, task_desc, NULL
    };
    int rc = exec_fire_and_forget(argv);

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

/* --- Fixup trigger (wall-clock, hourly) --- */

static time_t read_fixup_last_run(const char *nbs_root) {
    char path[4096];
    int n = snprintf(path, sizeof(path),
                     "%s/.nbs/fixup-last-run", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[32];
    time_t last = 0;
    if (fgets(buf, sizeof(buf), f)) {
        char *endptr;
        long long parsed = strtoll(buf, &endptr, 10);
        if (endptr != buf && parsed > 0)
            last = (time_t)parsed;
    }
    fclose(f);
    return last;
}

static void write_fixup_last_run(const char *nbs_root, time_t when) {
    char path[4096], tmp_path[4096];
    int n = snprintf(path, sizeof(path),
                     "%s/.nbs/fixup-last-run", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    n = snprintf(tmp_path, sizeof(tmp_path),
                 "%s/.nbs/fixup-last-run.tmp", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) return;

    FILE *f = fopen(tmp_path, "w");
    if (f) {
        fprintf(f, "%lld\n", (long long)when);
        if (fclose(f) == 0) {
            rename(tmp_path, path);
        } else {
            unlink(tmp_path);
        }
    }
}

int trigger_fixup_check(const char *nbs_root, int interval_secs) {
    ASSERT_MSG(nbs_root != NULL, "trigger_fixup_check: nbs_root is NULL");

    if (interval_secs <= 0) return 1;

    time_t now = time(NULL);
    time_t last_run = read_fixup_last_run(nbs_root);

    /* First run: initialise timestamp without firing */
    if (last_run == 0) {
        write_fixup_last_run(nbs_root, now);
        return 1;
    }

    if ((now - last_run) < interval_secs) {
        return 1;
    }

    /* Time elapsed — claim and spawn */
    write_fixup_last_run(nbs_root, now);
    trigger_fixup_spawn(nbs_root);
    return 0;
}

int trigger_fixup_spawn(const char *nbs_root) {
    ASSERT_MSG(nbs_root != NULL, "trigger_fixup_spawn: nbs_root is NULL");

    char lock_path[4096];
    int n = snprintf(lock_path, sizeof(lock_path),
                     "%s/.nbs/fixup.lock", nbs_root);
    ASSERT_MSG(n > 0 && (size_t)n < sizeof(lock_path),
               "trigger_fixup_spawn: lock path overflow");

    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "trigger_fixup_spawn: open lock failed: %s\n",
                strerror(errno));
        return -1;
    }

    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };

    if (fcntl(fd, F_SETLK, &fl) < 0) {
        close(fd);
        return 1;
    }

    const char *task_desc =
        "Load /nbs-fixup-auto. Run /nbs-teams-fixup on all agents. "
        "Post summary to chat. Exit.";

    const char *argv[] = {
        resolve_nbs_workers(), "spawn", "fixup", nbs_root, task_desc, NULL
    };
    int rc = exec_fire_and_forget(argv);

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
