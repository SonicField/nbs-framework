/*
 * triggers.c — Periodic trigger functions (pythia, shepard, fixup, librarian).
 *
 * All four triggers share the same pattern: shared timestamp file for
 * cross-sidecar dedup, lock-guarded fork+exec via nbs-workers. The
 * only differences are filenames, role name, and task description —
 * captured in the trigger_periodic_t config struct.
 */

/* _GNU_SOURCE required for SYS_gettid on Linux */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "triggers.h"
#include "../nbs-common/trigger_defs.h"
#include "bus_client.h"
#include "chat_client.h"
#include "exec_util.h"
#include "registry.h"
#include "../nbs-common/nbs_assert.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* --- Static trigger definitions --- */

const trigger_periodic_t TRIGGER_PYTHIA = {
    .name = "pythia",
    .ts_filename = "pythia-last-run",
    .lock_filename = "pythia.lock",
    .role = TRIGGER_ROLE_PYTHIA,
    .skill_file = TRIGGER_SKILL_PYTHIA,
    .task_desc = TRIGGER_DESC_PYTHIA,
    .first_delay_secs = 600,   /* 10 min — let scribe accumulate decisions first */
};

const trigger_periodic_t TRIGGER_SHEPARD = {
    .name = "shepard",
    .ts_filename = "shepard-last-run",
    .lock_filename = "shepard.lock",
    .role = TRIGGER_ROLE_SHEPARD,
    .skill_file = TRIGGER_SKILL_SHEPARD,
    .task_desc = TRIGGER_DESC_SHEPARD,
    .first_delay_secs = 600,   /* 10 min — let team settle first */
};

const trigger_periodic_t TRIGGER_FIXUP = {
    .name = "fixup",
    .ts_filename = "fixup-last-run",
    .lock_filename = "fixup.lock",
    .role = TRIGGER_ROLE_FIXUP,
    .skill_file = TRIGGER_SKILL_FIXUP,
    .task_desc = TRIGGER_DESC_FIXUP,
    .first_delay_secs = 600,   /* 10 min — agents need time to initialise */
};

const trigger_periodic_t TRIGGER_LIBRARIAN = {
    .name = "librarian",
    .ts_filename = "librarian-last-run",
    .lock_filename = "librarian.lock",
    .role = TRIGGER_ROLE_LIBRARIAN,
    .skill_file = TRIGGER_SKILL_LIBRARIAN,
    .task_desc = TRIGGER_DESC_LIBRARIAN,
    .first_delay_secs = 300,   /* 5 min — early first post to introduce herself */
};

/* --- nbs-workers path resolution --- */

/*
 * Cached absolute path to the nbs-workers binary.
 * Resolved once via /proc/self/exe — sibling binary in same directory.
 *
 * Not thread-safe by design — file-scope statics have no synchronisation.
 * Invariant enforced by ASSERT below: callers must be on the main thread
 * (where gettid() == getpid() on Linux). This is falsifiable — calling
 * from a spawned thread will fire the assertion.
 *
 * Architectural justification: sidecar.c:sidecar_run_loop() is the sole
 * caller of trigger_periodic_check/spawn, and that loop runs exclusively
 * on the main thread (no thread creation in sidecar event loop).
 */
#define WORKERS_PATH_LEN 4096
static char nbs_workers_path[WORKERS_PATH_LEN] = "";
static int nbs_workers_path_resolved = 0;

__attribute__((unused))
static const char *resolve_nbs_workers(void)
{
    ASSERT_MSG((pid_t)syscall(SYS_gettid) == getpid(),
               "resolve_nbs_workers: called from non-main thread (tid=%ld, pid=%d) "
               "— file-scope statics are not synchronised",
               (long)syscall(SYS_gettid), (int)getpid());
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

/* resolve_spawn_worker removed — nbs-workers spawn is the single
 * entry point for worker lifecycle. */

/* --- Shared timestamp file I/O --- */

static time_t read_last_run(const char *nbs_root, const char *ts_filename) {
    ASSERT_MSG(nbs_root != NULL, "read_last_run: nbs_root is NULL");
    ASSERT_MSG(ts_filename != NULL, "read_last_run: ts_filename is NULL");

    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/.nbs/%s", nbs_root, ts_filename);
    ASSERT_MSG(n > 0 && (size_t)n < sizeof(path),
               "read_last_run(%s): timestamp path overflow (%d >= %zu)",
               ts_filename, n, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT) {
            fprintf(stderr, "read_last_run(%s): fopen failed: %s\n",
                    ts_filename, strerror(errno));
        }
        return 0;
    }

    long long ts = 0;
    if (fscanf(f, "%lld", &ts) != 1) ts = 0;
    fclose(f);

    return (time_t)ts;
}

static void write_last_run(const char *nbs_root, const char *ts_filename,
                           time_t when) {
    ASSERT_MSG(nbs_root != NULL, "write_last_run: nbs_root is NULL");
    ASSERT_MSG(ts_filename != NULL, "write_last_run: ts_filename is NULL");

    char path[4096], tmp_path[4096];
    int n = snprintf(path, sizeof(path), "%s/.nbs/%s", nbs_root, ts_filename);
    ASSERT_MSG(n > 0 && (size_t)n < sizeof(path),
               "write_last_run(%s): timestamp path overflow (%d >= %zu)",
               ts_filename, n, sizeof(path));
    /* S1 fix: use mkstemp instead of predictable .tmp suffix to prevent
     * symlink attacks in shared directories. */
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.XXXXXX", path);
    ASSERT_MSG(tn > 0 && (size_t)tn < sizeof(tmp_path),
               "write_last_run(%s): tmp path overflow (%d >= %zu)",
               ts_filename, tn, sizeof(tmp_path));

    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd >= 0) {
        FILE *f = fdopen(tmp_fd, "w");
        if (!f) {
            close(tmp_fd);
            unlink(tmp_path);
            return;
        }
        if (fprintf(f, "%lld\n", (long long)when) < 0) {
            fclose(f);
            unlink(tmp_path);
            return;
        }
        if (fclose(f) == 0) {
            if (rename(tmp_path, path) != 0) {
                fprintf(stderr, "write_last_run(%s): rename failed: %s\n",
                        ts_filename, strerror(errno));
                unlink(tmp_path);
            }
        } else {
            unlink(tmp_path);
        }
    }
}

/* --- Generic periodic trigger --- */

int trigger_periodic_check(const char *nbs_root, int interval_secs,
                           const trigger_periodic_t *trigger) {
    ASSERT_MSG(nbs_root != NULL, "trigger_periodic_check: nbs_root is NULL");
    ASSERT_MSG(trigger != NULL, "trigger_periodic_check: trigger is NULL");
    ASSERT_MSG(interval_secs > 0,
               "trigger_periodic_check(%s): interval_secs must be positive, got %d",
               trigger->name, interval_secs);

    time_t now = time(NULL);
    time_t last_run = read_last_run(nbs_root, trigger->ts_filename);

    /* First run: initialise timestamp.
     * Seed to (now - interval + first_delay) so the first fire happens
     * after first_delay seconds, not after the full interval. This lets
     * agents settle before the first check while still firing early. */
    if (last_run == 0) {
        time_t seed = now - interval_secs + trigger->first_delay_secs;
        write_last_run(nbs_root, trigger->ts_filename, seed);
        return 1;
    }

    if ((now - last_run) < interval_secs) {
        return 1;
    }

    /*
     * Time elapsed — claim and spawn.
     *
     * There is a TOCTOU window between reading the timestamp (above)
     * and writing here. Two sidecars can both read the stale timestamp,
     * both pass the elapsed check, and both proceed. The lock in
     * trigger_periodic_spawn serialises the spawn command but does not
     * prevent sequential duplicate spawns. This is acceptable: all
     * periodic workers are idempotent, and duplicate runs are harmless
     * (just wasteful).
     */
    /* Do NOT write_last_run here — it must happen inside
     * trigger_periodic_spawn, after acquiring the lock. Otherwise
     * multiple sidecars all pass the elapsed check, all write the
     * timestamp, and all try to spawn (lock only serialises the
     * spawn, not the check). */
    trigger_periodic_spawn(nbs_root, trigger);
    return 0;
}

int trigger_periodic_spawn(const char *nbs_root,
                           const trigger_periodic_t *trigger) {
    ASSERT_MSG(nbs_root != NULL, "trigger_periodic_spawn: nbs_root is NULL");
    ASSERT_MSG(trigger != NULL, "trigger_periodic_spawn: trigger is NULL");

    /* Build lock path */
    char lock_path[4096];
    int n = snprintf(lock_path, sizeof(lock_path),
                     "%s/.nbs/%s", nbs_root, trigger->lock_filename);
    ASSERT_MSG(n > 0 && (size_t)n < sizeof(lock_path),
               "trigger_periodic_spawn(%s): lock path overflow",
               trigger->name);

    /* Non-blocking lock acquisition */
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "trigger_periodic_spawn(%s): open lock failed: %s\n",
                trigger->name, strerror(errno));
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

    /* Re-check timestamp after acquiring lock. Multiple sidecars can pass
     * the elapsed check in trigger_periodic_check before any acquires the
     * lock. The first winner writes the timestamp; subsequent winners must
     * re-read and bail if it's been updated. Without this, N sidecars
     * produce N duplicate spawns (observed: 3 librarian posts in 30s). */
    time_t recheck = read_last_run(nbs_root, trigger->ts_filename);
    if (recheck > 0 && (time(NULL) - recheck) < 30) {
        /* Another sidecar already spawned recently — bail */
        struct flock unlock = { .l_type = F_UNLCK, .l_whence = SEEK_SET };
        fcntl(fd, F_SETLK, &unlock);
        close(fd);
        return 1;
    }

    /* Write timestamp AFTER acquiring lock — only the winning sidecar
     * updates the timestamp, preventing duplicate spawns. */
    write_last_run(nbs_root, trigger->ts_filename, time(NULL));

    /* Fork+exec nbs-workers spawn. Single source of truth for
     * worker lifecycle — handles task file, session, naming. */
    char skill_flag[4096];
    snprintf(skill_flag, sizeof(skill_flag), "--skill=%s", trigger->skill_file);
    const char *argv[] = {
        resolve_nbs_workers(), "spawn", trigger->role,
        nbs_root, skill_flag, trigger->task_desc, NULL
    };
    int rc = exec_fire_and_forget(argv);

    /* Register oracle with the reaper for lifecycle management.
     * The reaper detects when the oracle posts to chat and kills
     * the session. Without this, oracles accumulate as zombies. */
    {
        char reaper_path[WORKERS_PATH_LEN];
        char self[WORKERS_PATH_LEN];
        ssize_t rlen = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (rlen > 0) {
            self[rlen] = '\0';
            char *rslash = strrchr(self, '/');
            if (rslash) {
                size_t rdir_len = (size_t)(rslash - self);
                if (rdir_len + sizeof("/nbs-oracle-reaper") <= sizeof(reaper_path)) {
                    memcpy(reaper_path, self, rdir_len);
                    memcpy(reaper_path + rdir_len, "/nbs-oracle-reaper",
                           sizeof("/nbs-oracle-reaper"));
                    const char *reaper_argv[] = {
                        reaper_path, "register", trigger->role,
                        nbs_root, NULL
                    };
                    exec_fire_and_forget(reaper_argv);
                }
            }
        }
    }

    /* Release lock */
    struct flock unlock = {
        .l_type = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    if (fcntl(fd, F_SETLK, &unlock) < 0) {
        fprintf(stderr, "trigger_periodic_spawn(%s): unlock failed: %s\n",
                trigger->name, strerror(errno));
    }
    close(fd);

    return (rc == 0) ? 0 : -1;
}
