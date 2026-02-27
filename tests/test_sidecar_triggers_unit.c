/*
 * test_sidecar_triggers_unit.c — Unit tests for triggers.c
 *
 * Falsifiable claims tested:
 *
 *   trigger_pythia_check:
 *    1. Below threshold, no action
 *    2. Crosses threshold, publishes
 *    3. First run syncs without firing
 *    4. Already past threshold, no re-fire
 *    5. Config file sets interval
 *   17. Cross-sidecar dedup: second sidecar suppressed by shared file
 *   18. Cross-sidecar dedup: new bucket after shared file triggers normally
 *   19. Cross-sidecar dedup: missing shared file does not suppress
 *
 *   trigger_standup_check:
 *    6. Interval not elapsed
 *    7. First run initialises timer
 *    8. Minimum valid interval (1 minute) — interval=0 now asserts
 *    9. CSMA/CD: recent global standup suppresses
 *   10. Posts after interval
 *
 *   trigger_heartbeat:
 *   11. Disabled (interval=0) — 0 is documented disable signal
 *   12. First run initialises
 *   13. Not elapsed
 *   14. Elapsed, posts
 *
 *   trigger_pythia_spawn:
 *   15. Lock acquired, spawn attempted
 *   16. Lock busy
 *
 *   trigger_shepard_check:
 *   20. Below threshold, no action
 *   21. Crosses threshold, fires
 *   22. First run syncs without firing
 *   23. Cross-sidecar dedup via shared file
 *
 *   trigger_shepard_spawn:
 *   24. Lock acquired
 *   25. Lock busy
 *
 *   trigger_fixup_check:
 *   26. Minimum valid interval (1s) — interval<=0 now asserts
 *   27. First run initialises timestamp
 *   28. Interval not elapsed
 *   29. Interval elapsed, fires
 *
 *   trigger_fixup_spawn:
 *   30. Lock acquired
 *   31. Lock busy
 *
 *   Adversarial tests (audit violations):
 *   32. BUG #5/#6: snprintf truncation assertions (build-time verified)
 *   33. HARDENING #9: count_dir_by_type postcondition (non-negative)
 *   34. HARDENING #3: heartbeat interval=0 returns 1 (disable signal)
 *   35. Pythia scribe-log fallback to bus events
 *   36. Fixup double-fire detection (timestamp updated after first fire)
 */

#include "../src/nbs-sidecar/triggers.h"
#include "../src/nbs-sidecar/registry.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- Test harness ---- */

static int tests = 0, fails = 0;
#define CHECK(label, cond) do { \
    tests++; \
    if (!(cond)) { \
        fails++; \
        printf("   FAIL: %s\n", label); \
    } else { \
        printf("   PASS: %s\n", label); \
    } \
} while(0)

/*
 * Buffer tiers (satisfies -Werror=format-truncation):
 *   L0 (128)  — tmpdir base (mkdtemp paths ~30 chars)
 *   L1 (256)  — nbs_root, registry_path
 *   L2 (512)  — subdirectories (nbs_root + "/.nbs/events/processed")
 *   L3 (768)  — file paths within subdirectories
 *   L4 (1024) — registry entries, commands
 */
#define L0 128
#define L1 256
#define L2 512
#define L3 768
#define L4 1024

/* ---- Helpers ---- */

static void mkdirs(const char *path)
{
    char tmp[L2];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void touch(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static void rmrf(const char *path)
{
    char cmd[L2];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

/*
 * create_pythia_env — Set up a temporary directory tree for pythia tests.
 *
 * Creates:
 *   <tmpdir>/project/.nbs/events/processed/   (bus dir + processed subdir)
 *   <tmpdir>/registry                          (registry file with bus: entry)
 *
 * Populates processed/ with n_events files named "decision-logged-<i>".
 */
static void create_pythia_env(const char *tmpdir,
                              char *nbs_root, size_t nr_size,
                              char *registry_path, size_t rp_size,
                              char *bus_dir, size_t bd_size,
                              int n_events)
{
    snprintf(nbs_root, nr_size, "%s/project", tmpdir);
    snprintf(bus_dir, bd_size, "%s/.nbs/events", nbs_root);

    char processed[L2];
    snprintf(processed, sizeof(processed), "%s/processed", bus_dir);
    mkdirs(processed);

    /* Create decision-logged event files */
    for (int i = 0; i < n_events; i++) {
        char fpath[L3];
        snprintf(fpath, sizeof(fpath), "%s/decision-logged-%04d",
                 processed, i);
        touch(fpath);
    }

    /* Write registry file with bus entry */
    snprintf(registry_path, rp_size, "%s/registry", tmpdir);
    char entry[L4];
    snprintf(entry, sizeof(entry), "bus:%s\n", bus_dir);
    write_file(registry_path, entry);
}

/*
 * create_standup_env — Set up a temporary directory tree for standup tests.
 *
 * Creates a real chat file via nbs-chat create and a registry pointing to it.
 */
static void create_standup_env(const char *tmpdir,
                               char *nbs_root, size_t nr_size,
                               char *registry_path, size_t rp_size,
                               char *chat_path, size_t cp_size)
{
    snprintf(nbs_root, nr_size, "%s/project", tmpdir);

    char chat_dir[L1];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", nbs_root);
    mkdirs(chat_dir);

    snprintf(chat_path, cp_size, "%s/test.chat", chat_dir);

    /* Create real chat file via nbs-chat */
    char cmd[L4];
    snprintf(cmd, sizeof(cmd), "nbs-chat create '%s' 2>/dev/null", chat_path);
    (void)system(cmd);

    /* Write registry */
    snprintf(registry_path, rp_size, "%s/registry", tmpdir);
    char entry[L4];
    snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
    write_file(registry_path, entry);
}

/* ---- Tests ---- */

int main(void)
{
    printf("test_sidecar_triggers_unit\n");

    /* Master temp directory */
    char master_tmp[] = "/tmp/nbs_trig_test_XXXXXX";
    if (!mkdtemp(master_tmp)) {
        fprintf(stderr, "Failed to create temp dir: %s\n", strerror(errno));
        return 1;
    }

    /* =================================================================
     * trigger_pythia_check tests
     * ================================================================= */
    printf("\n-- trigger_pythia_check --\n");

    /* Test 1: Below threshold, no action */
    {
        char sub[] = "/tmp/nbs_trig_t1_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        create_pythia_env(sub, nbs_root, sizeof(nbs_root),
                          registry, sizeof(registry),
                          bus_dir, sizeof(bus_dir), 5);

        int last = 0;
        int rc = trigger_pythia_check(registry, nbs_root, &last);
        /* 5 events, default interval=20: bucket 0/20 = 0, no crossing */
        /* First run with last=0 and count=5 > 0: sync path sets last=5 */
        CHECK("T1: below threshold returns 1", rc == 1);
        /* After sync, last is set to 5 (first-run catch-up) */
        /* Call again to verify no action */
        rc = trigger_pythia_check(registry, nbs_root, &last);
        CHECK("T1: second call still returns 1", rc == 1);
        CHECK("T1: last_trigger_count unchanged at 5", last == 5);

        rmrf(sub);
    }

    /* Test 2: Crosses threshold, publishes */
    {
        char sub[] = "/tmp/nbs_trig_t2_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        create_pythia_env(sub, nbs_root, sizeof(nbs_root),
                          registry, sizeof(registry),
                          bus_dir, sizeof(bus_dir), 20);

        /*
         * count=20, interval=20: current_bucket=1, last_bucket=0 => fires.
         * Need last non-zero (to skip sync path) and in bucket 0.
         */
        int last = 1;  /* bucket 0 */
        int rc = trigger_pythia_check(registry, nbs_root, &last);
        CHECK("T2: crosses threshold returns 0", rc == 0);
        CHECK("T2: last_trigger_count updated to 20", last == 20);

        rmrf(sub);
    }

    /* Test 3: First run syncs without firing */
    {
        char sub[] = "/tmp/nbs_trig_t3_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        create_pythia_env(sub, nbs_root, sizeof(nbs_root),
                          registry, sizeof(registry),
                          bus_dir, sizeof(bus_dir), 15);

        int last = 0;
        int rc = trigger_pythia_check(registry, nbs_root, &last);
        CHECK("T3: first run returns 1 (no fire)", rc == 1);
        CHECK("T3: last_trigger_count synced to 15", last == 15);

        rmrf(sub);
    }

    /* Test 4: Already past threshold, no re-fire */
    {
        char sub[] = "/tmp/nbs_trig_t4_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        create_pythia_env(sub, nbs_root, sizeof(nbs_root),
                          registry, sizeof(registry),
                          bus_dir, sizeof(bus_dir), 25);

        /* last=25, count=25: same bucket, no crossing */
        int last = 25;
        int rc = trigger_pythia_check(registry, nbs_root, &last);
        CHECK("T4: already past threshold returns 1", rc == 1);
        CHECK("T4: last_trigger_count unchanged at 25", last == 25);

        rmrf(sub);
    }

    /* Test 5: Config file sets interval */
    {
        char sub[] = "/tmp/nbs_trig_t5_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        create_pythia_env(sub, nbs_root, sizeof(nbs_root),
                          registry, sizeof(registry),
                          bus_dir, sizeof(bus_dir), 10);

        /* Write config.yaml with interval=10 into bus_dir */
        char config_path[L3];
        snprintf(config_path, sizeof(config_path), "%s/config.yaml", bus_dir);
        write_file(config_path, "pythia-interval: 10\n");

        /*
         * count=10, interval=10: current_bucket=1, last_bucket=0 => fires.
         * Need last non-zero and in bucket 0 to avoid sync path.
         */
        int last = 1;  /* bucket 0 for interval 10 */
        int rc = trigger_pythia_check(registry, nbs_root, &last);
        CHECK("T5: config interval=10, 10 events fires", rc == 0);
        CHECK("T5: last_trigger_count updated to 10", last == 10);

        rmrf(sub);
    }

    /* =================================================================
     * trigger_standup_check tests
     * ================================================================= */
    printf("\n-- trigger_standup_check --\n");

    /* Test 6: Interval not elapsed */
    {
        char sub[] = "/tmp/nbs_trig_t6_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        time_t now = time(NULL);
        time_t last_standup = now - 60;  /* 60 seconds ago, interval=15 min */
        int rc = trigger_standup_check(registry, nbs_root, "test-handle",
                                       15, &last_standup);
        CHECK("T6: interval not elapsed returns 1", rc == 1);

        rmrf(sub);
    }

    /* Test 7: First run initialises timer */
    {
        char sub[] = "/tmp/nbs_trig_t7_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        time_t last_standup = 0;
        time_t before = time(NULL);
        int rc = trigger_standup_check(registry, nbs_root, "test-handle",
                                       15, &last_standup);
        time_t after = time(NULL);
        CHECK("T7: first run returns 1 (no post)", rc == 1);
        CHECK("T7: last_standup_time set to ~now",
              last_standup >= before && last_standup <= after);

        rmrf(sub);
    }

    /* Test 8: Minimum valid interval (1 minute)
     *
     * interval_minutes=0 now triggers ASSERT (per HARDENING #2 fix —
     * non-positive is a caller bug, not a disable signal). Instead,
     * test that interval=1 works and doesn't fire when elapsed time
     * is well under 60 seconds. */
    {
        char sub[] = "/tmp/nbs_trig_t8_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        time_t now = time(NULL);
        time_t last_standup = now - 10;  /* 10s ago, interval=1 min */
        int rc = trigger_standup_check(registry, nbs_root, "test-handle",
                                       1, &last_standup);
        CHECK("T8: interval=1, not elapsed, returns 1", rc == 1);

        rmrf(sub);
    }

    /* Test 9: CSMA/CD: recent global standup suppresses */
    {
        char sub[] = "/tmp/nbs_trig_t9_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        /* Write a recent global standup timestamp file.
         * The standup trigger reads <chat_path>.standup-ts */
        char ts_file[L3];
        snprintf(ts_file, sizeof(ts_file), "%s.standup-ts", chat_path);
        time_t now = time(NULL);
        char ts_buf[32];
        snprintf(ts_buf, sizeof(ts_buf), "%ld\n", (long)(now - 60));
        write_file(ts_file, ts_buf);

        /* Set last_standup_time far enough in the past to trigger */
        time_t last_standup = now - 901;
        int rc = trigger_standup_check(registry, nbs_root, "test-handle",
                                       15, &last_standup);
        CHECK("T9: CSMA/CD recent global suppresses, returns 1", rc == 1);

        rmrf(sub);
    }

    /* Test 10: Posts after interval */
    {
        char sub[] = "/tmp/nbs_trig_t10_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        /* Ensure no global standup timestamp file exists */
        char ts_file[L3];
        snprintf(ts_file, sizeof(ts_file), "%s.standup-ts", chat_path);
        unlink(ts_file);

        /* Set last_standup_time > 15 min ago */
        time_t now = time(NULL);
        time_t last_standup = now - 901;
        int rc = trigger_standup_check(registry, nbs_root, "test-handle",
                                       15, &last_standup);
        CHECK("T10: posts after interval, returns 0", rc == 0);

        rmrf(sub);
    }

    /* =================================================================
     * trigger_heartbeat tests
     * ================================================================= */
    printf("\n-- trigger_heartbeat --\n");

    /* Test 11: Disabled (interval=0) */
    {
        char sub[] = "/tmp/nbs_trig_t11_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        time_t last_hb = 1;
        int rc = trigger_heartbeat(registry, "test-handle", 0, &last_hb);
        CHECK("T11: heartbeat disabled returns 1", rc == 1);

        rmrf(sub);
    }

    /* Test 12: First run initialises */
    {
        char sub[] = "/tmp/nbs_trig_t12_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        time_t last_hb = 0;
        time_t before = time(NULL);
        int rc = trigger_heartbeat(registry, "test-handle", 300, &last_hb);
        time_t after = time(NULL);
        CHECK("T12: first run returns 1", rc == 1);
        CHECK("T12: last_heartbeat_time set to ~now",
              last_hb >= before && last_hb <= after);

        rmrf(sub);
    }

    /* Test 13: Not elapsed */
    {
        char sub[] = "/tmp/nbs_trig_t13_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        time_t now = time(NULL);
        time_t last_hb = now - 10;  /* 10s ago, interval=300 */
        int rc = trigger_heartbeat(registry, "test-handle", 300, &last_hb);
        CHECK("T13: not elapsed returns 1", rc == 1);

        rmrf(sub);
    }

    /* Test 14: Elapsed, posts */
    {
        char sub[] = "/tmp/nbs_trig_t14_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        time_t now = time(NULL);
        time_t last_hb = now - 301;  /* > 300s interval */
        int rc = trigger_heartbeat(registry, "test-handle", 300, &last_hb);
        CHECK("T14: elapsed posts, returns 0", rc == 0);

        rmrf(sub);
    }

    /* =================================================================
     * trigger_pythia_spawn tests
     * ================================================================= */
    printf("\n-- trigger_pythia_spawn --\n");

    /* Test 15: Lock acquired, spawn attempted */
    {
        char sub[] = "/tmp/nbs_trig_t15_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        /*
         * nbs-workers is not set up, so exec will likely fail.
         * We verify that the function returns 0 (spawned) or -1
         * (exec failed after lock acquired), but NOT 1 (lock busy).
         */
        int rc = trigger_pythia_spawn(nbs_root);
        CHECK("T15: lock acquired (not busy)", rc != 1);

        /* Verify lock file was created */
        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path), "%s/pythia.lock", nbs_dir);
        struct stat st;
        CHECK("T15: lock file created", stat(lock_path, &st) == 0);

        rmrf(sub);
    }

    /* Test 16: Lock busy — parent holds lock, child gets busy */
    {
        char sub[] = "/tmp/nbs_trig_t16_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/pythia.lock", nbs_root);

        /* Parent acquires lock */
        int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            CHECK("T16: open lock file", 0);
        } else {
            struct flock fl = {
                .l_type = F_WRLCK,
                .l_whence = SEEK_SET,
                .l_start = 0,
                .l_len = 0,
            };
            int lrc = fcntl(fd, F_SETLK, &fl);
            CHECK("T16: parent acquires lock", lrc == 0);

            /*
             * Fork child to test lock contention.
             *
             * POSIX lock semantics after fork:
             *   - Locks are per-process, not per-fd
             *   - Child inherits fd but NOT the lock
             *   - Actually: child inherits lock too, but closing
             *     the inherited fd releases the child's locks
             *   - Parent's lock is unaffected
             *
             * So: child closes the inherited fd (releasing its
             * lock copy), then calls trigger_pythia_spawn which
             * tries F_SETLK — parent still holds the lock, child
             * gets EAGAIN => returns 1.
             */
            pid_t pid = fork();
            if (pid == 0) {
                /* Child: close inherited fd to release child's lock copy */
                close(fd);

                int rc = trigger_pythia_spawn(nbs_root);
                _exit(rc);
            } else if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
                int child_rc = -99;
                if (WIFEXITED(status))
                    child_rc = WEXITSTATUS(status);
                CHECK("T16: child gets lock busy (returns 1)",
                      child_rc == 1);
            } else {
                CHECK("T16: fork failed", 0);
            }

            /* Release parent lock */
            struct flock unlock = {
                .l_type = F_UNLCK,
                .l_whence = SEEK_SET,
                .l_start = 0,
                .l_len = 0,
            };
            fcntl(fd, F_SETLK, &unlock);
            close(fd);
        }

        rmrf(sub);
    }

    /* =================================================================
     * Cross-sidecar Pythia dedup tests (D-1902 / D-1771840755d)
     *
     * The bug: multiple sidecars each have independent in-memory
     * last_trigger_count. When a bucket boundary is crossed, ALL
     * sidecars independently detect it and fire. The fix: a shared
     * file (.nbs/pythia-last-bucket) records the last triggered
     * bucket, preventing duplicate triggers across sidecars.
     * ================================================================= */
    printf("\n-- trigger_pythia_check (cross-sidecar dedup) --\n");

    /* Test 17: Second sidecar suppressed by shared file */
    {
        char sub[] = "/tmp/nbs_trig_t17_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        create_pythia_env(sub, nbs_root, sizeof(nbs_root),
                          registry, sizeof(registry),
                          bus_dir, sizeof(bus_dir), 20);

        /*
         * Simulate sidecar A having already triggered bucket 1:
         * Write shared file with bucket=1, then call trigger_pythia_check
         * as sidecar B with last=1 (bucket 0). Sidecar B sees the bucket
         * crossing but the shared file says bucket 1 was already handled.
         */
        char shared_path[L3];
        snprintf(shared_path, sizeof(shared_path),
                 "%s/.nbs/pythia-last-bucket", nbs_root);
        write_file(shared_path, "1\n");

        int last = 1;  /* bucket 0 for interval 20 */
        int rc = trigger_pythia_check(registry, nbs_root, &last);
        CHECK("T17: second sidecar suppressed by shared file, returns 1",
              rc == 1);
        CHECK("T17: last_trigger_count still updated to 20", last == 20);

        rmrf(sub);
    }

    /* Test 18: New bucket after shared file triggers normally */
    {
        char sub[] = "/tmp/nbs_trig_t18_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        create_pythia_env(sub, nbs_root, sizeof(nbs_root),
                          registry, sizeof(registry),
                          bus_dir, sizeof(bus_dir), 40);

        /*
         * Shared file says bucket 1 was triggered. Decision count is 40,
         * so current_bucket=2 (40/20). Since 2 > 1, this is a new bucket
         * that hasn't been handled — should fire.
         */
        char shared_path[L3];
        snprintf(shared_path, sizeof(shared_path),
                 "%s/.nbs/pythia-last-bucket", nbs_root);
        write_file(shared_path, "1\n");

        int last = 21;  /* bucket 1 for interval 20 */
        int rc = trigger_pythia_check(registry, nbs_root, &last);
        CHECK("T18: new bucket fires despite shared file, returns 0",
              rc == 0);
        CHECK("T18: last_trigger_count updated to 40", last == 40);

        /* Verify shared file was updated to bucket 2 */
        FILE *f = fopen(shared_path, "r");
        int stored_bucket = -1;
        if (f) {
            char buf[32];
            if (fgets(buf, sizeof(buf), f)) {
                stored_bucket = atoi(buf);
            }
            fclose(f);
        }
        CHECK("T18: shared file updated to bucket 2", stored_bucket == 2);

        rmrf(sub);
    }

    /* Test 19: Missing shared file does not suppress (first trigger) */
    {
        char sub[] = "/tmp/nbs_trig_t19_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        create_pythia_env(sub, nbs_root, sizeof(nbs_root),
                          registry, sizeof(registry),
                          bus_dir, sizeof(bus_dir), 20);

        /* No shared file exists — first trigger should succeed */
        char shared_path[L3];
        snprintf(shared_path, sizeof(shared_path),
                 "%s/.nbs/pythia-last-bucket", nbs_root);
        unlink(shared_path);  /* ensure absent */

        int last = 1;  /* bucket 0 for interval 20 */
        int rc = trigger_pythia_check(registry, nbs_root, &last);
        CHECK("T19: missing shared file allows trigger, returns 0", rc == 0);
        CHECK("T19: last_trigger_count updated to 20", last == 20);

        /* Verify shared file was created with bucket 1 */
        FILE *f = fopen(shared_path, "r");
        CHECK("T19: shared file was created", f != NULL);
        if (f) {
            char buf[32];
            int stored_bucket = -1;
            if (fgets(buf, sizeof(buf), f)) {
                stored_bucket = atoi(buf);
            }
            fclose(f);
            CHECK("T19: shared file contains bucket 1", stored_bucket == 1);
        }

        rmrf(sub);
    }

    /* =================================================================
     * trigger_shepard_check tests
     *
     * Structurally identical to trigger_pythia_check but counts
     * "chat-message" events with default interval 100.
     * ================================================================= */
    printf("\n-- trigger_shepard_check --\n");

    /* Test 20: Below threshold, no action */
    {
        char sub[] = "/tmp/nbs_trig_t20_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        /* Reuse create_pythia_env but name events "chat-message-*" */
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        snprintf(bus_dir, sizeof(bus_dir), "%s/.nbs/events", nbs_root);
        char processed[L3];
        snprintf(processed, sizeof(processed), "%s/processed", bus_dir);
        mkdirs(processed);

        /* Create 10 chat-message events (default interval=100) */
        for (int i = 0; i < 10; i++) {
            char fpath[L4];
            snprintf(fpath, sizeof(fpath), "%s/chat-message-%04d",
                     processed, i);
            touch(fpath);
        }

        snprintf(registry, sizeof(registry), "%s/registry", sub);
        char entry[L4];
        snprintf(entry, sizeof(entry), "bus:%s\n", bus_dir);
        write_file(registry, entry);

        int last = 0;
        int rc = trigger_shepard_check(registry, nbs_root, &last);
        CHECK("T20: below threshold returns 1", rc == 1);
        /* First run syncs: last should be set to 10 */
        rc = trigger_shepard_check(registry, nbs_root, &last);
        CHECK("T20: second call still returns 1", rc == 1);
        CHECK("T20: last_trigger_count is 10", last == 10);

        rmrf(sub);
    }

    /* Test 21: Crosses threshold, fires */
    {
        char sub[] = "/tmp/nbs_trig_t21_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        snprintf(bus_dir, sizeof(bus_dir), "%s/.nbs/events", nbs_root);
        char processed[L3];
        snprintf(processed, sizeof(processed), "%s/processed", bus_dir);
        mkdirs(processed);

        /* Create 100 chat-message events — crosses bucket 0→1 */
        for (int i = 0; i < 100; i++) {
            char fpath[L4];
            snprintf(fpath, sizeof(fpath), "%s/chat-message-%04d",
                     processed, i);
            touch(fpath);
        }

        snprintf(registry, sizeof(registry), "%s/registry", sub);
        char entry[L4];
        snprintf(entry, sizeof(entry), "bus:%s\n", bus_dir);
        write_file(registry, entry);

        int last = 1;  /* bucket 0 for interval 100 */
        int rc = trigger_shepard_check(registry, nbs_root, &last);
        CHECK("T21: crosses threshold returns 0", rc == 0);
        CHECK("T21: last_trigger_count updated to 100", last == 100);

        rmrf(sub);
    }

    /* Test 22: First run syncs without firing */
    {
        char sub[] = "/tmp/nbs_trig_t22_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        snprintf(bus_dir, sizeof(bus_dir), "%s/.nbs/events", nbs_root);
        char processed[L3];
        snprintf(processed, sizeof(processed), "%s/processed", bus_dir);
        mkdirs(processed);

        for (int i = 0; i < 50; i++) {
            char fpath[L4];
            snprintf(fpath, sizeof(fpath), "%s/chat-message-%04d",
                     processed, i);
            touch(fpath);
        }

        snprintf(registry, sizeof(registry), "%s/registry", sub);
        char entry[L4];
        snprintf(entry, sizeof(entry), "bus:%s\n", bus_dir);
        write_file(registry, entry);

        int last = 0;
        int rc = trigger_shepard_check(registry, nbs_root, &last);
        CHECK("T22: first run returns 1 (sync only)", rc == 1);
        CHECK("T22: last_trigger_count synced to 50", last == 50);

        rmrf(sub);
    }

    /* Test 23: Cross-sidecar dedup via shared file */
    {
        char sub[] = "/tmp/nbs_trig_t23_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        snprintf(bus_dir, sizeof(bus_dir), "%s/.nbs/events", nbs_root);
        char processed[L3];
        snprintf(processed, sizeof(processed), "%s/processed", bus_dir);
        mkdirs(processed);

        for (int i = 0; i < 100; i++) {
            char fpath[L4];
            snprintf(fpath, sizeof(fpath), "%s/chat-message-%04d",
                     processed, i);
            touch(fpath);
        }

        snprintf(registry, sizeof(registry), "%s/registry", sub);
        char entry[L4];
        snprintf(entry, sizeof(entry), "bus:%s\n", bus_dir);
        write_file(registry, entry);

        /* Simulate another sidecar having already triggered bucket 1 */
        char shared_path[L3];
        snprintf(shared_path, sizeof(shared_path),
                 "%s/.nbs/shepard-last-bucket", nbs_root);
        write_file(shared_path, "1\n");

        int last = 1;  /* bucket 0 */
        int rc = trigger_shepard_check(registry, nbs_root, &last);
        CHECK("T23: shared file suppresses duplicate, returns 1", rc == 1);
        CHECK("T23: last_trigger_count still updated to 100", last == 100);

        rmrf(sub);
    }

    /* =================================================================
     * trigger_shepard_spawn tests
     * ================================================================= */
    printf("\n-- trigger_shepard_spawn --\n");

    /* Test 24: Lock acquired */
    {
        char sub[] = "/tmp/nbs_trig_t24_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        int rc = trigger_shepard_spawn(nbs_root);
        CHECK("T24: lock acquired (not busy)", rc != 1);

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path), "%s/shepard.lock", nbs_dir);
        struct stat st;
        CHECK("T24: lock file created", stat(lock_path, &st) == 0);

        rmrf(sub);
    }

    /* Test 25: Lock busy — parent holds lock, child gets busy */
    {
        char sub[] = "/tmp/nbs_trig_t25_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/shepard.lock", nbs_root);

        int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            CHECK("T25: open lock file", 0);
        } else {
            struct flock fl = {
                .l_type = F_WRLCK,
                .l_whence = SEEK_SET,
                .l_start = 0,
                .l_len = 0,
            };
            int lrc = fcntl(fd, F_SETLK, &fl);
            CHECK("T25: parent acquires lock", lrc == 0);

            pid_t pid = fork();
            if (pid == 0) {
                close(fd);
                int rc = trigger_shepard_spawn(nbs_root);
                _exit(rc);
            } else if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
                int child_rc = -99;
                if (WIFEXITED(status))
                    child_rc = WEXITSTATUS(status);
                CHECK("T25: child gets lock busy (returns 1)",
                      child_rc == 1);
            } else {
                CHECK("T25: fork failed", 0);
            }

            struct flock unlock = {
                .l_type = F_UNLCK,
                .l_whence = SEEK_SET,
                .l_start = 0,
                .l_len = 0,
            };
            fcntl(fd, F_SETLK, &unlock);
            close(fd);
        }

        rmrf(sub);
    }

    /* =================================================================
     * trigger_fixup_check tests
     * ================================================================= */
    printf("\n-- trigger_fixup_check --\n");

    /* Test 26: Minimum valid interval (1 second)
     *
     * interval_secs <= 0 now triggers ASSERT (per HARDENING #4 fix —
     * non-positive is a caller bug). Test that interval=1 works
     * correctly on first run (initialises without firing). */
    {
        char sub[] = "/tmp/nbs_trig_t26_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        /* Ensure no fixup-last-run file exists */
        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/fixup-last-run", nbs_root);
        unlink(ts_path);

        int rc = trigger_fixup_check(nbs_root, 1);
        CHECK("T26: interval=1 first run returns 1", rc == 1);

        rmrf(sub);
    }

    /* Test 27: First run initialises timestamp without firing */
    {
        char sub[] = "/tmp/nbs_trig_t27_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        /* Ensure no fixup-last-run file exists */
        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/fixup-last-run", nbs_root);
        unlink(ts_path);

        int rc = trigger_fixup_check(nbs_root, 3600);
        CHECK("T27: first run returns 1 (no fire)", rc == 1);

        /* Verify timestamp file was created */
        struct stat st;
        CHECK("T27: fixup-last-run file created",
              stat(ts_path, &st) == 0);

        rmrf(sub);
    }

    /* Test 28: Interval not elapsed, no action */
    {
        char sub[] = "/tmp/nbs_trig_t28_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        /* Write recent timestamp (10 seconds ago) */
        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/fixup-last-run", nbs_root);
        time_t now = time(NULL);
        char ts_buf[32];
        snprintf(ts_buf, sizeof(ts_buf), "%ld\n", (long)(now - 10));
        write_file(ts_path, ts_buf);

        int rc = trigger_fixup_check(nbs_root, 3600);
        CHECK("T28: interval not elapsed returns 1", rc == 1);

        rmrf(sub);
    }

    /* Test 29: Interval elapsed, fires */
    {
        char sub[] = "/tmp/nbs_trig_t29_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        /* Write old timestamp (2 hours ago, interval=3600) */
        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/fixup-last-run", nbs_root);
        time_t now = time(NULL);
        char ts_buf[32];
        snprintf(ts_buf, sizeof(ts_buf), "%ld\n", (long)(now - 7200));
        write_file(ts_path, ts_buf);

        int rc = trigger_fixup_check(nbs_root, 3600);
        CHECK("T29: interval elapsed, fires (returns 0)", rc == 0);

        /* Verify timestamp was updated */
        FILE *f = fopen(ts_path, "r");
        time_t updated_ts = 0;
        if (f) {
            char buf[32];
            if (fgets(buf, sizeof(buf), f))
                updated_ts = (time_t)atol(buf);
            fclose(f);
        }
        CHECK("T29: fixup-last-run updated to ~now",
              updated_ts >= now && updated_ts <= now + 2);

        rmrf(sub);
    }

    /* =================================================================
     * trigger_fixup_spawn tests
     * ================================================================= */
    printf("\n-- trigger_fixup_spawn --\n");

    /* Test 30: Lock acquired */
    {
        char sub[] = "/tmp/nbs_trig_t30_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        int rc = trigger_fixup_spawn(nbs_root);
        CHECK("T30: lock acquired (not busy)", rc != 1);

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path), "%s/fixup.lock", nbs_dir);
        struct stat st;
        CHECK("T30: lock file created", stat(lock_path, &st) == 0);

        rmrf(sub);
    }

    /* Test 31: Lock busy — contention */
    {
        char sub[] = "/tmp/nbs_trig_t31_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/fixup.lock", nbs_root);

        int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            CHECK("T31: open lock file", 0);
        } else {
            struct flock fl = {
                .l_type = F_WRLCK,
                .l_whence = SEEK_SET,
                .l_start = 0,
                .l_len = 0,
            };
            int lrc = fcntl(fd, F_SETLK, &fl);
            CHECK("T31: parent acquires lock", lrc == 0);

            pid_t pid = fork();
            if (pid == 0) {
                close(fd);
                int rc = trigger_fixup_spawn(nbs_root);
                _exit(rc);
            } else if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
                int child_rc = -99;
                if (WIFEXITED(status))
                    child_rc = WEXITSTATUS(status);
                CHECK("T31: child gets lock busy (returns 1)",
                      child_rc == 1);
            } else {
                CHECK("T31: fork failed", 0);
            }

            struct flock unlock = {
                .l_type = F_UNLCK,
                .l_whence = SEEK_SET,
                .l_start = 0,
                .l_len = 0,
            };
            fcntl(fd, F_SETLK, &unlock);
            close(fd);
        }

        rmrf(sub);
    }

    /* =================================================================
     * Adversarial tests (audit violation fixes)
     * ================================================================= */
    printf("\n-- Adversarial tests (audit fixes) --\n");

    /* Test 32: snprintf truncation assertions verified at compile time.
     *
     * BUG #5/#6 fix added ASSERT_MSG after snprintf for ts_file and
     * ts_tmp. This test verifies the normal path works — the assert
     * fires only if truncation occurs, which requires paths > 4096 chars
     * (not constructible in this test environment without hitting OS
     * PATH_MAX limits). The assertion is the falsifier — if it ever
     * fires, we know the buffer sizing assumptions are wrong.
     *
     * Here we verify normal operation with a realistic path. */
    {
        char sub[] = "/tmp/nbs_trig_t32_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        /* Ensure no global standup timestamp */
        char ts_file[L3];
        snprintf(ts_file, sizeof(ts_file), "%s.standup-ts", chat_path);
        unlink(ts_file);

        time_t now = time(NULL);
        time_t last_standup = now - 901;
        int rc = trigger_standup_check(registry, nbs_root, "test-handle",
                                       15, &last_standup);
        CHECK("T32: standup with snprintf assertions succeeds", rc == 0);

        /* Verify the .standup-ts file was created (ts_file path works) */
        struct stat st;
        CHECK("T32: standup-ts file created", stat(ts_file, &st) == 0);

        rmrf(sub);
    }

    /* Test 33: HARDENING #9 — count_dir_by_type returns non-negative.
     *
     * Verify count is correct for an empty directory and a populated one.
     * The postcondition assert in count_dir_by_type fires if count < 0. */
    {
        char sub[] = "/tmp/nbs_trig_t33_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        /* Create env with 0 events — count should be 0, not negative */
        create_pythia_env(sub, nbs_root, sizeof(nbs_root),
                          registry, sizeof(registry),
                          bus_dir, sizeof(bus_dir), 0);

        int last = 0;
        int rc = trigger_pythia_check(registry, nbs_root, &last);
        /* No events, no bus events, no scribe decisions — should be clean */
        CHECK("T33: zero events, no action", rc == 1);
        CHECK("T33: last_trigger_count stays 0", last == 0);

        rmrf(sub);
    }

    /* Test 34: HARDENING #3 — heartbeat interval=0 returns 1 (disable).
     *
     * Verify that interval=0 is still the documented disable signal
     * and does not trigger the assert (which is interval >= 0). */
    {
        char sub[] = "/tmp/nbs_trig_t34_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], chat_path[L2];
        create_standup_env(sub, nbs_root, sizeof(nbs_root),
                           registry, sizeof(registry),
                           chat_path, sizeof(chat_path));

        time_t last_hb = 1;
        int rc = trigger_heartbeat(registry, "test-handle", 0, &last_hb);
        CHECK("T34: heartbeat interval=0 returns 1 (disabled)", rc == 1);

        rmrf(sub);
    }

    /* Test 35: Pythia falls back to bus events when scribe log is empty.
     *
     * count_scribe_decisions returns 0 when the log doesn't exist,
     * so the code falls back to counting decision-logged bus events.
     * Verify this fallback path works. */
    {
        char sub[] = "/tmp/nbs_trig_t35_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1], registry[L1], bus_dir[L2];
        /* Create 20 decision-logged events (crosses bucket 0→1) */
        create_pythia_env(sub, nbs_root, sizeof(nbs_root),
                          registry, sizeof(registry),
                          bus_dir, sizeof(bus_dir), 20);

        /* Ensure no scribe log exists — forces fallback to bus events */
        char scribe_log[L3];
        snprintf(scribe_log, sizeof(scribe_log),
                 "%s/.nbs/scribe/live-log.md", nbs_root);
        unlink(scribe_log);

        int last = 1;  /* bucket 0 */
        int rc = trigger_pythia_check(registry, nbs_root, &last);
        CHECK("T35: pythia fires on bus events (scribe fallback)", rc == 0);
        CHECK("T35: last_trigger_count updated to 20", last == 20);

        rmrf(sub);
    }

    /* Test 36: Fixup double-fire detection — timestamp updated after fire.
     *
     * After trigger_fixup_check fires (returns 0), the timestamp file
     * should be updated to ~now. A second call immediately after should
     * return 1 (not enough time elapsed). */
    {
        char sub[] = "/tmp/nbs_trig_t36_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }

        char nbs_root[L1];
        snprintf(nbs_root, sizeof(nbs_root), "%s/project", sub);
        char nbs_dir[L2];
        snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
        mkdirs(nbs_dir);

        /* Write old timestamp */
        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/fixup-last-run", nbs_root);
        time_t now = time(NULL);
        char ts_buf[32];
        snprintf(ts_buf, sizeof(ts_buf), "%ld\n", (long)(now - 7200));
        write_file(ts_path, ts_buf);

        /* First call should fire */
        int rc1 = trigger_fixup_check(nbs_root, 3600);
        CHECK("T36: first call fires (returns 0)", rc1 == 0);

        /* Second call immediately after: timestamp was just updated */
        int rc2 = trigger_fixup_check(nbs_root, 3600);
        CHECK("T36: second call suppressed (returns 1)", rc2 == 1);

        rmrf(sub);
    }

    /* Clean up master temp directory */
    rmrf(master_tmp);

    printf("\n%d/%d passed\n", tests - fails, tests);
    return fails;
}
