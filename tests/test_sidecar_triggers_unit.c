/*
 * test_sidecar_triggers_unit.c — Unit tests for triggers.c
 *
 * Falsifiable claims tested:
 *
 *   trigger_periodic_check (tested via TRIGGER_PYTHIA):
 *    1. First run initialises timestamp without firing
 *    2. Interval not elapsed, no action
 *    3. Interval elapsed, fires
 *    4. Double-fire suppression
 *    5. Minimum valid interval (1 second)
 *
 *   trigger_periodic_spawn (tested via TRIGGER_PYTHIA):
 *    6. Lock acquired, spawn attempted
 *    7. Lock busy (contention)
 *
 *   trigger_periodic_check (tested via TRIGGER_SHEPARD):
 *    8. First run initialises timestamp
 *    9. Interval not elapsed
 *   10. Interval elapsed, fires
 *   11. Double-fire suppression
 *
 *   trigger_periodic_spawn (tested via TRIGGER_SHEPARD):
 *   12. Lock acquired
 *   13. Lock busy
 *
 *   trigger_periodic_check (tested via TRIGGER_FIXUP):
 *   14. Minimum valid interval (1 second)
 *   15. First run initialises timestamp
 *   16. Interval not elapsed
 *   17. Interval elapsed, fires + timestamp updated
 *   18. Double-fire suppression
 *
 *   trigger_periodic_spawn (tested via TRIGGER_FIXUP):
 *   19. Lock acquired
 *   20. Lock busy
 *
 *   trigger_periodic_check (tested via TRIGGER_LIBRARIAN):
 *   21. First run initialises timestamp
 *   22. Interval not elapsed
 *   23. Interval elapsed, fires + timestamp updated
 *   24. Double-fire suppression
 *   25. Minimum valid interval (1 second)
 *
 *   trigger_periodic_spawn (tested via TRIGGER_LIBRARIAN):
 *   26. Lock acquired
 *   27. Lock busy
 *
 *   Adversarial tests:
 *   28. Corrupted timestamp file (non-numeric) — treated as first-run
 *   29. Future timestamp — suppresses until real time catches up
 *   30. interval=0 — ASSERT fires (abort)
 */

#include "../src/nbs-sidecar/triggers.h"

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
 *   L1 (256)  — nbs_root
 *   L2 (512)  — subdirectories (nbs_root + "/.nbs/...")
 *   L3 (768)  — file paths within subdirectories
 */
#define L1 256
#define L2 512
#define L3 768

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
 * create_nbs_env — Set up a minimal .nbs directory for trigger tests.
 */
static void create_nbs_env(const char *tmpdir, char *nbs_root, size_t nr_size)
{
    snprintf(nbs_root, nr_size, "%s/project", tmpdir);
    char nbs_dir[L2];
    snprintf(nbs_dir, sizeof(nbs_dir), "%s/.nbs", nbs_root);
    mkdirs(nbs_dir);
}

/*
 * write_timestamp — Write a timestamp file under .nbs/.
 */
static void write_timestamp(const char *nbs_root, const char *ts_filename,
                            time_t when)
{
    char ts_path[L3];
    snprintf(ts_path, sizeof(ts_path), "%s/.nbs/%s", nbs_root, ts_filename);
    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "%ld\n", (long)when);
    write_file(ts_path, ts_buf);
}

/*
 * test_lock_busy — Fork a child to test lock contention.
 *
 * Parent holds lock on lock_path. Child closes inherited fd (releasing
 * its lock copy per POSIX semantics), then calls trigger_periodic_spawn.
 * Parent's lock is unaffected — child should get EAGAIN and return 1.
 */
static void test_lock_busy(const char *label, const char *nbs_root,
                           const char *lock_path,
                           const trigger_periodic_t *trigger)
{
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        CHECK(label, 0);
        return;
    }

    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    int lrc = fcntl(fd, F_SETLK, &fl);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: parent acquires lock", label);
    CHECK(buf, lrc == 0);

    pid_t pid = fork();
    if (pid == 0) {
        close(fd);
        int rc = trigger_periodic_spawn(nbs_root, trigger);
        _exit(rc);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        int child_rc = -99;
        if (WIFEXITED(status))
            child_rc = WEXITSTATUS(status);
        snprintf(buf, sizeof(buf), "%s: child gets lock busy (returns 1)",
                 label);
        CHECK(buf, child_rc == 1);
    } else {
        snprintf(buf, sizeof(buf), "%s: fork failed", label);
        CHECK(buf, 0);
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

/* ---- Tests ---- */

/*
 * strip_nbs_from_path — Remove nbs-framework/bin from PATH.
 *
 * The spawn functions fork+exec nbs-workers. If nbs-workers is in PATH,
 * the spawned worker runs forever and the test hangs. Stripping these
 * directories ensures exec fails cleanly, which is what the tests expect.
 */
static void strip_nbs_from_path(void)
{
    const char *path = getenv("PATH");
    if (!path) return;

    char new_path[8192];
    new_path[0] = '\0';
    size_t off = 0;

    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);

        char component[4096];
        if (len < sizeof(component)) {
            memcpy(component, p, len);
            component[len] = '\0';
            if (!strstr(component, "nbs-framework/bin") &&
                !strstr(component, ".nbs/bin")) {
                if (off > 0 && off < sizeof(new_path) - 1)
                    new_path[off++] = ':';
                size_t avail = sizeof(new_path) - off - 1;
                if (len < avail) {
                    memcpy(new_path + off, p, len);
                    off += len;
                }
            }
        }

        p += len;
        if (*p == ':') p++;
    }
    new_path[off] = '\0';
    setenv("PATH", new_path, 1);
}

int main(void)
{
    printf("test_sidecar_triggers_unit\n");

    strip_nbs_from_path();

    char master_tmp[] = "/tmp/nbs_trig_test_XXXXXX";
    if (!mkdtemp(master_tmp)) {
        fprintf(stderr, "Failed to create temp dir: %s\n", strerror(errno));
        return 1;
    }

    /* =================================================================
     * trigger_periodic_check tests (via TRIGGER_PYTHIA)
     * ================================================================= */
    printf("\n-- trigger_periodic_check (pythia) --\n");

    /* T1: First run initialises timestamp */
    {
        char sub[] = "/tmp/nbs_trig_t1_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_check(nbs_root, 1800, &TRIGGER_PYTHIA);
        CHECK("T1: first run returns 1 (no fire)", rc == 1);

        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path), "%s/.nbs/pythia-last-run", nbs_root);
        struct stat st;
        CHECK("T1: timestamp file created", stat(ts_path, &st) == 0);
        rmrf(sub);
    }

    /* T2: Interval not elapsed */
    {
        char sub[] = "/tmp/nbs_trig_t2_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "pythia-last-run", time(NULL) - 10);

        int rc = trigger_periodic_check(nbs_root, 1800, &TRIGGER_PYTHIA);
        CHECK("T2: interval not elapsed returns 1", rc == 1);
        rmrf(sub);
    }

    /* T3: Interval elapsed, fires */
    {
        char sub[] = "/tmp/nbs_trig_t3_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "pythia-last-run", time(NULL) - 2000);

        int rc = trigger_periodic_check(nbs_root, 1800, &TRIGGER_PYTHIA);
        CHECK("T3: interval elapsed returns 0 (fires)", rc == 0);
        rmrf(sub);
    }

    /* T4: Double-fire suppression */
    {
        char sub[] = "/tmp/nbs_trig_t4_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "pythia-last-run", time(NULL) - 2000);

        int rc1 = trigger_periodic_check(nbs_root, 1800, &TRIGGER_PYTHIA);
        CHECK("T4: first call fires (returns 0)", rc1 == 0);
        int rc2 = trigger_periodic_check(nbs_root, 1800, &TRIGGER_PYTHIA);
        CHECK("T4: second call suppressed (returns 1)", rc2 == 1);
        rmrf(sub);
    }

    /* T5: Minimum valid interval (1 second) */
    {
        char sub[] = "/tmp/nbs_trig_t5_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_check(nbs_root, 1, &TRIGGER_PYTHIA);
        CHECK("T5: interval=1 first run returns 1", rc == 1);
        rmrf(sub);
    }

    /* =================================================================
     * trigger_periodic_spawn tests (via TRIGGER_PYTHIA)
     * ================================================================= */
    printf("\n-- trigger_periodic_spawn (pythia) --\n");

    /* T6: Lock acquired, spawn attempted */
    {
        char sub[] = "/tmp/nbs_trig_t6_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_spawn(nbs_root, &TRIGGER_PYTHIA);
        CHECK("T6: lock acquired (not busy)", rc != 1);

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/pythia.lock", nbs_root);
        struct stat st;
        CHECK("T6: lock file created", stat(lock_path, &st) == 0);
        rmrf(sub);
    }

    /* T7: Lock busy */
    {
        char sub[] = "/tmp/nbs_trig_t7_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/pythia.lock", nbs_root);
        test_lock_busy("T7", nbs_root, lock_path, &TRIGGER_PYTHIA);
        rmrf(sub);
    }

    /* =================================================================
     * trigger_periodic_check tests (via TRIGGER_SHEPARD)
     * ================================================================= */
    printf("\n-- trigger_periodic_check (shepard) --\n");

    /* T8: First run */
    {
        char sub[] = "/tmp/nbs_trig_t8_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_check(nbs_root, 1800, &TRIGGER_SHEPARD);
        CHECK("T8: first run returns 1 (no fire)", rc == 1);

        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/shepard-last-run", nbs_root);
        struct stat st;
        CHECK("T8: timestamp file created", stat(ts_path, &st) == 0);
        rmrf(sub);
    }

    /* T9: Interval not elapsed */
    {
        char sub[] = "/tmp/nbs_trig_t9_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "shepard-last-run", time(NULL) - 10);

        int rc = trigger_periodic_check(nbs_root, 1800, &TRIGGER_SHEPARD);
        CHECK("T9: interval not elapsed returns 1", rc == 1);
        rmrf(sub);
    }

    /* T10: Interval elapsed */
    {
        char sub[] = "/tmp/nbs_trig_t10_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "shepard-last-run", time(NULL) - 2000);

        int rc = trigger_periodic_check(nbs_root, 1800, &TRIGGER_SHEPARD);
        CHECK("T10: interval elapsed returns 0", rc == 0);
        rmrf(sub);
    }

    /* T11: Double-fire suppression */
    {
        char sub[] = "/tmp/nbs_trig_t11_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "shepard-last-run", time(NULL) - 2000);

        int rc1 = trigger_periodic_check(nbs_root, 1800, &TRIGGER_SHEPARD);
        CHECK("T11: first call fires", rc1 == 0);
        int rc2 = trigger_periodic_check(nbs_root, 1800, &TRIGGER_SHEPARD);
        CHECK("T11: second call suppressed", rc2 == 1);
        rmrf(sub);
    }

    /* =================================================================
     * trigger_periodic_spawn tests (via TRIGGER_SHEPARD)
     * ================================================================= */
    printf("\n-- trigger_periodic_spawn (shepard) --\n");

    /* T12: Lock acquired */
    {
        char sub[] = "/tmp/nbs_trig_t12_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_spawn(nbs_root, &TRIGGER_SHEPARD);
        CHECK("T12: lock acquired (not busy)", rc != 1);

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/shepard.lock", nbs_root);
        struct stat st;
        CHECK("T12: lock file created", stat(lock_path, &st) == 0);
        rmrf(sub);
    }

    /* T13: Lock busy */
    {
        char sub[] = "/tmp/nbs_trig_t13_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/shepard.lock", nbs_root);
        test_lock_busy("T13", nbs_root, lock_path, &TRIGGER_SHEPARD);
        rmrf(sub);
    }

    /* =================================================================
     * trigger_periodic_check tests (via TRIGGER_FIXUP)
     * ================================================================= */
    printf("\n-- trigger_periodic_check (fixup) --\n");

    /* T14: Minimum valid interval (1 second) */
    {
        char sub[] = "/tmp/nbs_trig_t14_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_check(nbs_root, 1, &TRIGGER_FIXUP);
        CHECK("T14: interval=1 first run returns 1", rc == 1);
        rmrf(sub);
    }

    /* T15: First run initialises timestamp */
    {
        char sub[] = "/tmp/nbs_trig_t15_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_check(nbs_root, 3600, &TRIGGER_FIXUP);
        CHECK("T15: first run returns 1 (no fire)", rc == 1);

        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/fixup-last-run", nbs_root);
        struct stat st;
        CHECK("T15: timestamp file created", stat(ts_path, &st) == 0);
        rmrf(sub);
    }

    /* T16: Interval not elapsed */
    {
        char sub[] = "/tmp/nbs_trig_t16_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "fixup-last-run", time(NULL) - 10);

        int rc = trigger_periodic_check(nbs_root, 3600, &TRIGGER_FIXUP);
        CHECK("T16: interval not elapsed returns 1", rc == 1);
        rmrf(sub);
    }

    /* T17: Interval elapsed, fires + timestamp updated */
    {
        char sub[] = "/tmp/nbs_trig_t17_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        time_t now = time(NULL);
        write_timestamp(nbs_root, "fixup-last-run", now - 7200);

        int rc = trigger_periodic_check(nbs_root, 3600, &TRIGGER_FIXUP);
        CHECK("T17: interval elapsed, fires (returns 0)", rc == 0);

        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/fixup-last-run", nbs_root);
        FILE *f = fopen(ts_path, "r");
        time_t updated_ts = 0;
        if (f) {
            char buf[32];
            if (fgets(buf, sizeof(buf), f))
                updated_ts = (time_t)atol(buf);
            fclose(f);
        }
        CHECK("T17: timestamp updated to ~now",
              updated_ts >= now && updated_ts <= now + 2);
        rmrf(sub);
    }

    /* T18: Double-fire suppression */
    {
        char sub[] = "/tmp/nbs_trig_t18_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "fixup-last-run", time(NULL) - 7200);

        int rc1 = trigger_periodic_check(nbs_root, 3600, &TRIGGER_FIXUP);
        CHECK("T18: first call fires", rc1 == 0);
        int rc2 = trigger_periodic_check(nbs_root, 3600, &TRIGGER_FIXUP);
        CHECK("T18: second call suppressed", rc2 == 1);
        rmrf(sub);
    }

    /* =================================================================
     * trigger_periodic_spawn tests (via TRIGGER_FIXUP)
     * ================================================================= */
    printf("\n-- trigger_periodic_spawn (fixup) --\n");

    /* T19: Lock acquired */
    {
        char sub[] = "/tmp/nbs_trig_t19_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_spawn(nbs_root, &TRIGGER_FIXUP);
        CHECK("T19: lock acquired (not busy)", rc != 1);

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/fixup.lock", nbs_root);
        struct stat st;
        CHECK("T19: lock file created", stat(lock_path, &st) == 0);
        rmrf(sub);
    }

    /* T20: Lock busy */
    {
        char sub[] = "/tmp/nbs_trig_t20_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/fixup.lock", nbs_root);
        test_lock_busy("T20", nbs_root, lock_path, &TRIGGER_FIXUP);
        rmrf(sub);
    }

    /* =================================================================
     * trigger_periodic_check tests (via TRIGGER_LIBRARIAN)
     * ================================================================= */
    printf("\n-- trigger_periodic_check (librarian) --\n");

    /* T21: First run */
    {
        char sub[] = "/tmp/nbs_trig_t21_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_check(nbs_root, 900, &TRIGGER_LIBRARIAN);
        CHECK("T21: first run returns 1 (no fire)", rc == 1);

        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/librarian-last-run", nbs_root);
        struct stat st;
        CHECK("T21: timestamp file created", stat(ts_path, &st) == 0);
        rmrf(sub);
    }

    /* T22: Interval not elapsed */
    {
        char sub[] = "/tmp/nbs_trig_t22_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "librarian-last-run", time(NULL) - 10);

        int rc = trigger_periodic_check(nbs_root, 900, &TRIGGER_LIBRARIAN);
        CHECK("T22: interval not elapsed returns 1", rc == 1);
        rmrf(sub);
    }

    /* T23: Interval elapsed, fires + timestamp updated */
    {
        char sub[] = "/tmp/nbs_trig_t23_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        time_t now = time(NULL);
        write_timestamp(nbs_root, "librarian-last-run", now - 1000);

        int rc = trigger_periodic_check(nbs_root, 900, &TRIGGER_LIBRARIAN);
        CHECK("T23: interval elapsed returns 0 (fires)", rc == 0);

        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/librarian-last-run", nbs_root);
        FILE *f = fopen(ts_path, "r");
        long long new_ts = 0;
        if (f) { fscanf(f, "%lld", &new_ts); fclose(f); }
        CHECK("T23: timestamp updated to ~now",
              llabs(new_ts - (long long)now) < 5);
        rmrf(sub);
    }

    /* T24: Double-fire suppression */
    {
        char sub[] = "/tmp/nbs_trig_t24_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "librarian-last-run", time(NULL) - 1000);

        int rc1 = trigger_periodic_check(nbs_root, 900, &TRIGGER_LIBRARIAN);
        CHECK("T24: first call fires", rc1 == 0);
        int rc2 = trigger_periodic_check(nbs_root, 900, &TRIGGER_LIBRARIAN);
        CHECK("T24: second call suppressed", rc2 == 1);
        rmrf(sub);
    }

    /* T25: Minimum valid interval (1 second) */
    {
        char sub[] = "/tmp/nbs_trig_t25_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_check(nbs_root, 1, &TRIGGER_LIBRARIAN);
        CHECK("T25: interval=1 first run returns 1", rc == 1);
        rmrf(sub);
    }

    /* =================================================================
     * trigger_periodic_spawn tests (via TRIGGER_LIBRARIAN)
     * ================================================================= */
    printf("\n-- trigger_periodic_spawn (librarian) --\n");

    /* T26: Lock acquired */
    {
        char sub[] = "/tmp/nbs_trig_t26_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        int rc = trigger_periodic_spawn(nbs_root, &TRIGGER_LIBRARIAN);
        CHECK("T26: lock acquired (not busy)", rc != 1);

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/librarian.lock", nbs_root);
        struct stat st;
        CHECK("T26: lock file created", stat(lock_path, &st) == 0);
        rmrf(sub);
    }

    /* T27: Lock busy */
    {
        char sub[] = "/tmp/nbs_trig_t27_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        char lock_path[L3];
        snprintf(lock_path, sizeof(lock_path),
                 "%s/.nbs/librarian.lock", nbs_root);
        test_lock_busy("T27", nbs_root, lock_path, &TRIGGER_LIBRARIAN);
        rmrf(sub);
    }

    /* =================================================================
     * Adversarial tests
     * ================================================================= */
    printf("\n-- Adversarial tests --\n");

    /* T28: Corrupted timestamp file (non-numeric) */
    {
        char sub[] = "/tmp/nbs_trig_t28_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        char ts_path[L3];
        snprintf(ts_path, sizeof(ts_path),
                 "%s/.nbs/librarian-last-run", nbs_root);
        write_file(ts_path, "not-a-number\n");

        /* fscanf fails → returns 0 → treated as first-run */
        int rc = trigger_periodic_check(nbs_root, 900, &TRIGGER_LIBRARIAN);
        CHECK("T28: corrupted timestamp treated as first-run (returns 1)",
              rc == 1);
        rmrf(sub);
    }

    /* T29: Future timestamp suppresses */
    {
        char sub[] = "/tmp/nbs_trig_t29_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        write_timestamp(nbs_root, "librarian-last-run", time(NULL) + 3600);

        /* Future timestamp: now - future is negative → elapsed < interval */
        int rc = trigger_periodic_check(nbs_root, 900, &TRIGGER_LIBRARIAN);
        CHECK("T29: future timestamp suppresses (returns 1)", rc == 1);
        rmrf(sub);
    }

    /* T30: interval=0 assertion fires */
    {
        char sub[] = "/tmp/nbs_trig_t30_XXXXXX";
        if (!mkdtemp(sub)) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
        char nbs_root[L1];
        create_nbs_env(sub, nbs_root, sizeof(nbs_root));

        pid_t pid = fork();
        if (pid == 0) {
            trigger_periodic_check(nbs_root, 0, &TRIGGER_LIBRARIAN);
            _exit(0); /* should not reach here */
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            CHECK("T30: interval=0 aborts (SIGABRT)",
                  WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        } else {
            CHECK("T30: fork failed", 0);
        }
        rmrf(sub);
    }

    /* Clean up */
    rmrf(master_tmp);

    printf("\n%d/%d passed\n", tests - fails, tests);
    return fails;
}
