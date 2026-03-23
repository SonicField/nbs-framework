/*
 * test_nbs_ts.c — C unit tests for the nbs-ts library API.
 *
 * Tests session.c, io.c, wait.c directly via the public API in nbs_ts.h.
 * Compiled with -DTEST_BUILD to exclude main() from main.c.
 *
 * Build: gcc -Wall -Wextra -Wshadow -Werror -std=c11 -DTEST_BUILD -O2
 *        -I../nbs-common test_nbs_ts.c session.c io.c wait.c -lpthread -lutil
 */

#include "nbs_ts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

static int pass_count = 0;
static int fail_count = 0;

#define PASS(msg) do { printf("   PASS: %s\n", msg); pass_count++; } while(0)
#define FAIL(msg) do { printf("   FAIL: %s\n", msg); fail_count++; } while(0)
#define CHECK(cond, msg) do { if (cond) PASS(msg); else FAIL(msg); } while(0)

/* U1: nbs_ts_create succeeds */
static void test_create(void)
{
    printf("U1. Create session...\n");
    nbs_ts_session_t *s = nbs_ts_create("echo hello", NULL);
    CHECK(s != NULL, "create returns non-NULL");
    if (s) {
        CHECK(nbs_ts_handle(s) != NULL, "handle is non-NULL");
        CHECK(strlen(nbs_ts_handle(s)) == 8, "handle is 8 chars");
        CHECK(nbs_ts_pid(s) > 0, "PID is positive");
        nbs_ts_destroy(s);
    }
}

/* U3: send + read-new round-trip */
static void test_send_read(void)
{
    printf("U3. Send and read-new...\n");
    nbs_ts_session_t *s = nbs_ts_create("bash", NULL);
    CHECK(s != NULL, "create bash session");
    if (!s) return;

    usleep(500000); /* let bash start */

    int rc = nbs_ts_send(s, "echo UNIT_MARKER_42\n", 20);
    CHECK(rc == 0, "send returns 0");

    usleep(500000); /* let output arrive */

    char buf[4096];
    size_t n = nbs_ts_read_new(s, buf, sizeof(buf) - 1);
    CHECK(n > 0, "read-new returns bytes");
    if (n > 0) {
        buf[n] = '\0';
        CHECK(strstr(buf, "UNIT_MARKER_42") != NULL, "output contains marker");
    }

    nbs_ts_destroy(s);
}

/* U7: read-new returns 0 on second call without new output */
static void test_read_new_idempotent(void)
{
    printf("U7. Read-new idempotent...\n");
    nbs_ts_session_t *s = nbs_ts_create("echo ONCE", NULL);
    CHECK(s != NULL, "create session");
    if (!s) return;

    usleep(500000);

    char buf[4096];
    size_t n1 = nbs_ts_read_new(s, buf, sizeof(buf));
    CHECK(n1 > 0, "first read-new returns bytes");

    size_t n2 = nbs_ts_read_new(s, buf, sizeof(buf));
    CHECK(n2 == 0, "second read-new returns 0 (no new output)");

    nbs_ts_destroy(s);
}

/* U8: read with offset */
static void test_read_offset(void)
{
    printf("U8. Read with offset...\n");
    nbs_ts_session_t *s = nbs_ts_create("echo OFFSET_TEST", NULL);
    CHECK(s != NULL, "create session");
    if (!s) return;

    usleep(500000);

    char buf1[4096], buf2[4096];
    size_t n1 = nbs_ts_read(s, buf1, sizeof(buf1), 0);
    CHECK(n1 > 0, "read from offset 0 returns bytes");

    size_t n2 = nbs_ts_read(s, buf2, sizeof(buf2), 0);
    CHECK(n2 == n1, "second read from offset 0 returns same length");

    if (n1 > 0 && n2 > 0 && n1 == n2) {
        CHECK(memcmp(buf1, buf2, n1) == 0, "same content on repeated reads");
    }

    nbs_ts_destroy(s);
}

/* U14: status returns alive for running session */
static void test_status_alive(void)
{
    printf("U14. Status alive...\n");
    nbs_ts_session_t *s = nbs_ts_create("bash", NULL);
    CHECK(s != NULL, "create bash session");
    if (!s) return;

    usleep(200000);
    nbs_ts_status_t st = nbs_ts_status(s);
    CHECK(st == NBS_TS_ALIVE, "status is alive");

    nbs_ts_destroy(s);
}

/* U15: status returns dead after exit */
static void test_status_dead(void)
{
    printf("U15. Status dead after exit...\n");
    nbs_ts_session_t *s = nbs_ts_create("exit 0", NULL);
    CHECK(s != NULL, "create exit session");
    if (!s) return;

    usleep(1000000); /* let it exit */
    nbs_ts_status_t st = nbs_ts_status(s);
    CHECK(st == NBS_TS_DEAD, "status is dead");

    nbs_ts_destroy(s);
}

/* U16: destroy cleans up */
static void test_destroy_cleanup(void)
{
    printf("U16. Destroy cleans up...\n");
    nbs_ts_session_t *s = nbs_ts_create("bash", NULL);
    CHECK(s != NULL, "create session");
    if (!s) return;

    const char *handle = nbs_ts_handle(s);
    char dir[NBS_TS_MAX_PATH];
    nbs_ts_session_dir(handle, dir, sizeof(dir));

    nbs_ts_destroy(s);

    /* Check session directory is gone */
    struct stat st;
    CHECK(stat(dir, &st) != 0, "session directory removed after destroy");
}

/* U17: destroy on already-dead session */
static void test_destroy_dead(void)
{
    printf("U17. Destroy already-dead session...\n");
    nbs_ts_session_t *s = nbs_ts_create("exit 0", NULL);
    CHECK(s != NULL, "create exit session");
    if (!s) return;

    usleep(1000000); /* let it exit */

    /* Should not crash */
    nbs_ts_destroy(s);
    PASS("destroy on dead session did not crash");
}

/* U21: PID is valid */
static void test_pid_valid(void)
{
    printf("U21. PID is valid...\n");
    nbs_ts_session_t *s = nbs_ts_create("bash", NULL);
    CHECK(s != NULL, "create session");
    if (!s) return;

    pid_t pid = nbs_ts_pid(s);
    CHECK(pid > 0, "PID is positive");
    CHECK(kill(pid, 0) == 0, "PID is alive (kill -0 succeeds)");

    nbs_ts_destroy(s);
}

/* read_tail test */
static void test_read_tail(void)
{
    printf("RT. Read tail...\n");
    nbs_ts_session_t *s = nbs_ts_create("bash", NULL);
    CHECK(s != NULL, "create session");
    if (!s) return;

    usleep(500000);
    nbs_ts_send(s, "for i in $(seq 1 20); do echo LINE_$i; done\n", 44);
    usleep(1000000);

    char buf[4096];
    size_t n = nbs_ts_read_tail(s, buf, sizeof(buf) - 1, 5);
    CHECK(n > 0, "read_tail returns bytes");
    if (n > 0) {
        buf[n] = '\0';
        CHECK(strstr(buf, "LINE_20") != NULL, "tail contains LINE_20");
        CHECK(strstr(buf, "LINE_1\n") == NULL, "tail does not contain LINE_1");
    }

    nbs_ts_destroy(s);
}

int main(void)
{
    printf("=== nbs-ts C Unit Tests ===\n\n");

    test_create();
    test_send_read();
    test_read_new_idempotent();
    test_read_offset();
    test_status_alive();
    test_status_dead();
    test_destroy_cleanup();
    test_destroy_dead();
    test_pid_valid();
    test_read_tail();

    printf("\n=== Result ===\n");
    if (fail_count == 0) {
        printf("PASS: All %d C unit tests passed\n", pass_count);
        return 0;
    } else {
        printf("FAIL: %d passed, %d failed\n", pass_count, fail_count);
        return 1;
    }
}
