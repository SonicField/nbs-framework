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
 *
 * HARDENING #8: Invariant: resolve_nbs_workers must only be called from
 * the main event loop thread. Not thread-safe by design — file-scope
 * statics have no synchronisation.
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

/* --- Pythia timer helpers --- */

static time_t read_pythia_last_run(const char *nbs_root) {
    ASSERT_MSG(nbs_root != NULL, "read_pythia_last_run: nbs_root is NULL");
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/.nbs/pythia-last-run", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long long ts = 0;
    if (fscanf(f, "%lld", &ts) != 1) ts = 0;
    fclose(f);
    return (time_t)ts;
}

static void write_pythia_last_run(const char *nbs_root, time_t when) {
    ASSERT_MSG(nbs_root != NULL, "write_pythia_last_run: nbs_root is NULL");
    char path[4096], tmp_path[4096];
    int n = snprintf(path, sizeof(path), "%s/.nbs/pythia-last-run", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp_path)) return;
    FILE *f = fopen(tmp_path, "w");
    if (f) {
        fprintf(f, "%lld\n", (long long)when);
        fclose(f);
        if (rename(tmp_path, path) != 0) {
            fprintf(stderr, "write_pythia_last_run: rename failed: %s\n",
                    strerror(errno));
            unlink(tmp_path);
        }
    }
}

int trigger_pythia_check(const char *nbs_root, int interval_secs) {
    ASSERT_MSG(nbs_root != NULL, "trigger_pythia_check: nbs_root is NULL");
    ASSERT_MSG(interval_secs > 0,
               "trigger_pythia_check: interval_secs must be positive, got %d",
               interval_secs);

    time_t now = time(NULL);
    time_t last_run = read_pythia_last_run(nbs_root);

    if (last_run == 0) {
        write_pythia_last_run(nbs_root, now);
        return 1;
    }

    if ((now - last_run) < interval_secs) {
        return 1;
    }

    write_pythia_last_run(nbs_root, now);
    trigger_pythia_spawn(nbs_root);
    return 0;
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

    /* HARDENING #2 + BUG #1 fix: validate interval_minutes range.
     * Non-positive is a caller bug, not a disable signal.
     * Upper bound of 1440 (24h) prevents integer overflow in * 60. */
    ASSERT_MSG(interval_minutes > 0 && interval_minutes <= 1440,
               "trigger_standup_check: interval_minutes out of range: %d",
               interval_minutes);

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
    int ts_n = snprintf(ts_file, sizeof(ts_file), "%s.standup-ts", chat_path);
    /* BUG #5 fix: assert snprintf did not truncate */
    ASSERT_MSG(ts_n > 0 && (size_t)ts_n < sizeof(ts_file),
               "trigger_standup_check: ts_file path truncated");

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
            ssize_t rn = read(ufd, &rand_val, sizeof(rand_val));
            if (rn != (ssize_t)sizeof(rand_val)) {
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
            "on? What is blocked? Reminder: if you claim the human said "
            "something, quote it with timestamp. No quote = no evidence.";
    }

    /* Post standups as "sidecar", not as the agent's handle.
     * Using the agent's handle makes it look like the agent wrote the
     * standup, which confuses both the agent (sees messages it didn't
     * write under its own name) and other agents (read the backlog
     * and treat it as a coordinator directive to emulate). */
    chat_client_send(chat_path, "sidecar", standup_msg);

    /* Update shared timestamp atomically */
    char ts_tmp[4096 + 24];
    int ts_tmp_n = snprintf(ts_tmp, sizeof(ts_tmp), "%s.tmp", ts_file);
    /* BUG #6 fix: assert snprintf did not truncate */
    ASSERT_MSG(ts_tmp_n > 0 && (size_t)ts_tmp_n < sizeof(ts_tmp),
               "trigger_standup_check: ts_tmp path truncated");
    FILE *wtf = fopen(ts_tmp, "w");
    if (wtf) {
        if (fprintf(wtf, "%ld\n", (long)now) < 0) {
            fclose(wtf);
            unlink(ts_tmp);
        } else if (fclose(wtf) == 0) {
            if (rename(ts_tmp, ts_file) != 0) {
                fprintf(stderr, "trigger_standup_check: rename failed: %s\n",
                        strerror(errno));
                unlink(ts_tmp);
            }
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

    /* HARDENING #3 fix: negative interval is a caller bug.
     * interval=0 is a documented disable signal. */
    ASSERT_MSG(interval >= 0,
               "trigger_heartbeat: interval must be non-negative, got %d", interval);
    if (interval == 0) return 1;

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
    if (fcntl(fd, F_SETLK, &unlock) < 0) {
        fprintf(stderr, "trigger_pythia_spawn: unlock failed: %s\n",
                strerror(errno));
    }
    close(fd);

    return (rc == 0) ? 0 : -1;
}

/* --- Shepard trigger --- */

/* --- Shepard timer helpers --- */

static time_t read_shepard_last_run(const char *nbs_root) {
    ASSERT_MSG(nbs_root != NULL, "read_shepard_last_run: nbs_root is NULL");
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/.nbs/shepard-last-run", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long long ts = 0;
    if (fscanf(f, "%lld", &ts) != 1) ts = 0;
    fclose(f);
    return (time_t)ts;
}

static void write_shepard_last_run(const char *nbs_root, time_t when) {
    ASSERT_MSG(nbs_root != NULL, "write_shepard_last_run: nbs_root is NULL");
    char path[4096], tmp_path[4096];
    int n = snprintf(path, sizeof(path), "%s/.nbs/shepard-last-run", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp_path)) return;
    FILE *f = fopen(tmp_path, "w");
    if (f) {
        fprintf(f, "%lld\n", (long long)when);
        fclose(f);
        if (rename(tmp_path, path) != 0) {
            fprintf(stderr, "write_shepard_last_run: rename failed: %s\n",
                    strerror(errno));
            unlink(tmp_path);
        }
    }
}

int trigger_shepard_check(const char *nbs_root, int interval_secs) {
    ASSERT_MSG(nbs_root != NULL, "trigger_shepard_check: nbs_root is NULL");
    ASSERT_MSG(interval_secs > 0,
               "trigger_shepard_check: interval_secs must be positive, got %d",
               interval_secs);

    time_t now = time(NULL);
    time_t last_run = read_shepard_last_run(nbs_root);

    if (last_run == 0) {
        write_shepard_last_run(nbs_root, now);
        return 1;
    }

    if ((now - last_run) < interval_secs) {
        return 1;
    }

    write_shepard_last_run(nbs_root, now);
    trigger_shepard_spawn(nbs_root);
    return 0;
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
    if (fcntl(fd, F_SETLK, &unlock) < 0) {
        fprintf(stderr, "trigger_shepard_spawn: unlock failed: %s\n",
                strerror(errno));
    }
    close(fd);

    return (rc == 0) ? 0 : -1;
}

/* --- Fixup trigger (wall-clock, hourly) --- */

static time_t read_fixup_last_run(const char *nbs_root) {
    /* HARDENING #13 fix: precondition on nbs_root */
    ASSERT_MSG(nbs_root != NULL, "read_fixup_last_run: nbs_root is NULL");

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
    /* HARDENING #13 fix: precondition on nbs_root */
    ASSERT_MSG(nbs_root != NULL, "write_fixup_last_run: nbs_root is NULL");

    char path[4096], tmp_path[4096];
    int n = snprintf(path, sizeof(path),
                     "%s/.nbs/fixup-last-run", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    n = snprintf(tmp_path, sizeof(tmp_path),
                 "%s/.nbs/fixup-last-run.tmp", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) return;

    FILE *f = fopen(tmp_path, "w");
    if (f) {
        if (fprintf(f, "%lld\n", (long long)when) < 0) {
            fclose(f);
            unlink(tmp_path);
            return;
        }
        if (fclose(f) == 0) {
            if (rename(tmp_path, path) != 0) {
                fprintf(stderr, "write_fixup_last_run: rename failed: %s\n",
                        strerror(errno));
                unlink(tmp_path);
            }
        } else {
            unlink(tmp_path);
        }
    }
}

int trigger_fixup_check(const char *nbs_root, int interval_secs) {
    ASSERT_MSG(nbs_root != NULL, "trigger_fixup_check: nbs_root is NULL");

    /* HARDENING #4 fix: non-positive interval is a caller bug */
    ASSERT_MSG(interval_secs > 0,
               "trigger_fixup_check: interval_secs must be positive, got %d",
               interval_secs);

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

    /* Time elapsed — claim and spawn.
     *
     * BUG #7 acknowledgement: there is a TOCTOU window between reading
     * the timestamp (above) and writing here. Two sidecars can both read
     * the stale timestamp, both pass the elapsed check, and both proceed.
     * The lock in trigger_fixup_spawn serialises the spawn command but
     * does not prevent sequential duplicate spawns. This is acceptable:
     * fixup runs are idempotent, and duplicate runs are harmless (just
     * wasteful). A bucket-number scheme (like Pythia/Shepard) would
     * eliminate duplicates but adds complexity disproportionate to the
     * risk for an hourly-cadence trigger. */
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
    if (fcntl(fd, F_SETLK, &unlock) < 0) {
        fprintf(stderr, "trigger_fixup_spawn: unlock failed: %s\n",
                strerror(errno));
    }
    close(fd);

    return (rc == 0) ? 0 : -1;
}

/* --- Librarian trigger --- */

/*
 * Librarian is a timer-based institutional memory watchdog.
 * Same pattern as fixup: shared timestamp file + lock-guarded spawn.
 * Reads recent chat, searches scribe log, posts findings with @team!.
 */

static time_t read_librarian_last_run(const char *nbs_root) {
    ASSERT_MSG(nbs_root != NULL, "read_librarian_last_run: nbs_root is NULL");

    char path[4096];
    int n = snprintf(path, sizeof(path),
                     "%s/.nbs/librarian-last-run", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    long long ts = 0;
    if (fscanf(f, "%lld", &ts) != 1) ts = 0;
    fclose(f);

    return (time_t)ts;
}

static void write_librarian_last_run(const char *nbs_root, time_t when) {
    ASSERT_MSG(nbs_root != NULL, "write_librarian_last_run: nbs_root is NULL");

    char path[4096], tmp_path[4096];
    int n = snprintf(path, sizeof(path),
                     "%s/.nbs/librarian-last-run", nbs_root);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp_path)) return;

    FILE *f = fopen(tmp_path, "w");
    if (f) {
        fprintf(f, "%lld\n", (long long)when);
        fclose(f);
        if (rename(tmp_path, path) != 0) {
            fprintf(stderr, "write_librarian_last_run: rename failed: %s\n",
                    strerror(errno));
            unlink(tmp_path);
        }
    } else {
        unlink(tmp_path);
    }
}

int trigger_librarian_check(const char *nbs_root, int interval_secs) {
    ASSERT_MSG(nbs_root != NULL, "trigger_librarian_check: nbs_root is NULL");
    ASSERT_MSG(interval_secs > 0,
               "trigger_librarian_check: interval_secs must be positive, got %d",
               interval_secs);

    time_t now = time(NULL);
    time_t last_run = read_librarian_last_run(nbs_root);

    /* First run: initialise timestamp without firing */
    if (last_run == 0) {
        write_librarian_last_run(nbs_root, now);
        return 1;
    }

    if ((now - last_run) < interval_secs) {
        return 1;
    }

    /* Time elapsed — claim and spawn.
     * Same TOCTOU acknowledgement as fixup: duplicate runs are possible
     * but harmless (librarian posts are read-only assessments). */
    write_librarian_last_run(nbs_root, now);
    trigger_librarian_spawn(nbs_root);
    return 0;
}

int trigger_librarian_spawn(const char *nbs_root) {
    ASSERT_MSG(nbs_root != NULL, "trigger_librarian_spawn: nbs_root is NULL");

    char lock_path[4096];
    int n = snprintf(lock_path, sizeof(lock_path),
                     "%s/.nbs/librarian.lock", nbs_root);
    ASSERT_MSG(n > 0 && (size_t)n < sizeof(lock_path),
               "trigger_librarian_spawn: lock path overflow");

    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "trigger_librarian_spawn: open lock failed: %s\n",
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
        "Load /nbs-librarian. Read last 100 chat messages via nbs-chat read. "
        "Search scribe log for answers to questions or blockers the team is "
        "stuck on. Post findings with @team! tag. If scribe has nothing "
        "relevant, stay silent. Exit.";

    const char *argv[] = {
        resolve_nbs_workers(), "spawn", "librarian", nbs_root, task_desc, NULL
    };
    int rc = exec_fire_and_forget(argv);

    struct flock unlock = {
        .l_type = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    if (fcntl(fd, F_SETLK, &unlock) < 0) {
        fprintf(stderr, "trigger_librarian_spawn: unlock failed: %s\n",
                strerror(errno));
    }
    close(fd);

    return (rc == 0) ? 0 : -1;
}
