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
    .role = "pythia",
    .task_desc =
        "Load /nbs-pythia. Read .nbs/scribe/live-log.md. "
        "Run the checkpoint procedure. Post assessment to chat. Exit.",
};

const trigger_periodic_t TRIGGER_SHEPARD = {
    .name = "shepard",
    .ts_filename = "shepard-last-run",
    .lock_filename = "shepard.lock",
    .role = "shepard",
    .task_desc =
        "Load /nbs-shepard. Check agent liveness via nbs-workers list and "
        "nbs-workers status — classify each as healthy/stressed/zombie/dead. "
        "Read recent chat via sub-agents. Assess team effectiveness. "
        "Post recommendations to supervisor (agent status FIRST). Exit.",
};

const trigger_periodic_t TRIGGER_FIXUP = {
    .name = "fixup",
    .ts_filename = "fixup-last-run",
    .lock_filename = "fixup.lock",
    .role = "fixup",
    .task_desc =
        "Load /nbs-fixup-auto. Run /nbs-teams-fixup on all agents. "
        "Post summary to chat. Exit.",
};

const trigger_periodic_t TRIGGER_LIBRARIAN = {
    .name = "librarian",
    .ts_filename = "librarian-last-run",
    .lock_filename = "librarian.lock",
    .role = "librarian",
    .task_desc =
        "Load /nbs-librarian. Read last 100 chat messages via nbs-chat read. "
        "Search scribe log for answers to questions or blockers the team is "
        "stuck on. Post findings with @team! tag. If scribe has nothing "
        "relevant, stay silent. Exit.",
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
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    ASSERT_MSG(tn > 0 && (size_t)tn < sizeof(tmp_path),
               "write_last_run(%s): tmp path overflow (%d >= %zu)",
               ts_filename, tn, sizeof(tmp_path));

    FILE *f = fopen(tmp_path, "w");
    if (f) {
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

    /* First run: initialise timestamp without firing */
    if (last_run == 0) {
        write_last_run(nbs_root, trigger->ts_filename, now);
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
    write_last_run(nbs_root, trigger->ts_filename, now);
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

    /* Fork+exec nbs-workers spawn */
    const char *argv[] = {
        resolve_nbs_workers(), "spawn", trigger->role,
        nbs_root, trigger->task_desc, NULL
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
        fprintf(stderr, "trigger_periodic_spawn(%s): unlock failed: %s\n",
                trigger->name, strerror(errno));
    }
    close(fd);

    return (rc == 0) ? 0 : -1;
}
