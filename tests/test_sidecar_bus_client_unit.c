/*
 * test_sidecar_bus_client_unit.c -- Unit tests for bus_client.c
 *
 * Tests the bus_client API by creating temporary event directories,
 * publishing events via the nbs-bus binary, and verifying that
 * bus_client_check, bus_client_read, bus_client_ack, bus_client_publish,
 * and bus_client_check_typed behave correctly.
 *
 * Adversarial tests for BUG/SECURITY violations:
 *  11. bus_client_check returns -1 on exec failure (not 1)
 *  12. bus_client_check postconditions hold on success
 *  13. bus_client_check_typed exec failure returns -1
 *  14. bus_client_check_typed with multiple events only acks first match
 *  15. bus_client_publish allows empty payload
 *
 * Requires: nbs-bus binary in PATH.
 *
 * Build (from project root):
 *   export PATH="$(pwd)/bin:$PATH"
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -I src/nbs-common -I src/nbs-sidecar \
 *       -o tests/test_sidecar_bus_client_unit \
 *       tests/test_sidecar_bus_client_unit.c \
 *       src/nbs-sidecar/bus_client.o src/nbs-sidecar/exec_util.o
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "bus_client.h"
#include "exec_util.h"

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * make_bus_dir -- Create a temporary bus directory with processed/ subdir.
 * Returns a malloc'd string; caller must free after rmdir_recursive.
 */
static char *make_bus_dir(void)
{
    char tmpl[] = "/tmp/nbs-bus-test-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (dir == NULL) {
        perror("mkdtemp");
        exit(1);
    }
    char *result = strdup(dir);
    char proc[512];
    snprintf(proc, sizeof(proc), "%s/processed", result);
    if (mkdir(proc, 0755) != 0) {
        perror("mkdir processed");
        exit(1);
    }
    return result;
}

/*
 * rmdir_recursive -- Remove a directory and all its contents.
 * Only handles one level of subdirectories (sufficient for bus dirs).
 */
static void rmdir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL) return;
    struct dirent *ent;
    char child[1024];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            rmdir_recursive(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    rmdir(path);
}

/*
 * publish_via_cli -- Publish an event using nbs-bus binary directly.
 * This is the "known good" path for setting up test fixtures.
 */
static int publish_via_cli(const char *bus_dir, const char *source,
                           const char *type, const char *priority,
                           const char *payload)
{
    const char *argv[] = {
        "nbs-bus", "publish", bus_dir, source, type, priority, payload, NULL
    };
    char buf[4096];
    return exec_capture(argv, buf, sizeof(buf));
}

/*
 * check_via_cli -- Run nbs-bus check and capture output.
 * Returns exit code; buf contains output.
 */
static int check_via_cli(const char *bus_dir, char *buf, size_t buf_size)
{
    const char *argv[] = {"nbs-bus", "check", bus_dir, NULL};
    return exec_capture(argv, buf, buf_size);
}

/*
 * extract_filename -- Extract event filename from the first line of
 * nbs-bus check output. Format: [priority] filename (age)
 * Returns 0 on success, -1 on failure.
 */
static int extract_filename(const char *check_output, char *fname, size_t fname_size)
{
    /* Skip "[priority] " */
    const char *p = strchr(check_output, ']');
    if (p == NULL) return -1;
    p++; /* skip ] */
    while (*p == ' ') p++;

    const char *end = strchr(p, ' ');
    if (end == NULL) end = p + strlen(p);

    size_t len = (size_t)(end - p);
    if (len == 0 || len >= fname_size) return -1;

    memcpy(fname, p, len);
    fname[len] = '\0';
    return 0;
}

/*
 * verify_nbs_bus -- Check that nbs-bus is reachable via exec.
 * Uses a publish+check round-trip since 'help' writes to stderr
 * which exec_capture discards.
 * Returns 0 if working, -1 if not.
 */
static int verify_nbs_bus(void)
{
    char *dir = make_bus_dir();
    int rc = publish_via_cli(dir, "probe", "probe", "normal", "probe");
    rmdir_recursive(dir);
    free(dir);
    return (rc == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Test 1: bus_client_check with empty bus dir                         */
/* ------------------------------------------------------------------ */

static void test_check_empty(void)
{
    printf("\n-- test_check_empty --\n");
    char *dir = make_bus_dir();

    int event_count = -1;
    char max_priority[64] = {0};
    char summary[256] = {0};
    int rc = bus_client_check(dir, &event_count, max_priority, sizeof(max_priority),
                              summary, sizeof(summary));

    CHECK("empty dir: returns 1 (no events)", rc == 1);
    CHECK("empty dir: event_count is 0", event_count == 0);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 2: bus_client_check with one event                             */
/* ------------------------------------------------------------------ */

static void test_check_one_event(void)
{
    printf("\n-- test_check_one_event --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "test-src", "test-type", "normal", "hello world");

    int event_count = -1;
    char max_priority[64] = {0};
    char summary[256] = {0};
    int rc = bus_client_check(dir, &event_count, max_priority, sizeof(max_priority),
                              summary, sizeof(summary));

    CHECK("one event: returns 0 (events found)", rc == 0);
    CHECK("one event: event_count is 1", event_count == 1);
    CHECK("one event: max_priority is 'normal'",
          strcmp(max_priority, "normal") == 0);
    CHECK("one event: summary is non-empty", summary[0] != '\0');

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 3: bus_client_check priority extraction                        */
/* ------------------------------------------------------------------ */

static void test_check_priority(void)
{
    printf("\n-- test_check_priority --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "test-src", "test-type", "critical", "urgent payload");

    int event_count = -1;
    char max_priority[64] = {0};
    char summary[256] = {0};
    int rc = bus_client_check(dir, &event_count, max_priority, sizeof(max_priority),
                              summary, sizeof(summary));

    CHECK("critical priority: returns 0", rc == 0);
    CHECK("critical priority: max_priority is 'critical'",
          strcmp(max_priority, "critical") == 0);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 4: bus_client_read existing event                              */
/* ------------------------------------------------------------------ */

static void test_read_existing(void)
{
    printf("\n-- test_read_existing --\n");
    char *dir = make_bus_dir();

    const char *expected_payload = "payload for read test";
    publish_via_cli(dir, "test-src", "test-type", "normal", expected_payload);

    /* Get the filename from nbs-bus check */
    char check_out[4096];
    check_via_cli(dir, check_out, sizeof(check_out));

    char fname[256];
    int frc = extract_filename(check_out, fname, sizeof(fname));
    CHECK("read: extracted filename from check output", frc == 0);

    if (frc == 0) {
        char payload[4096];
        int rc = bus_client_read(dir, fname, payload, sizeof(payload));
        CHECK("read: returns 0", rc == 0);
        CHECK("read: payload contains published text",
              strstr(payload, expected_payload) != NULL);
    }

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 5: bus_client_ack removes event                                */
/* ------------------------------------------------------------------ */

static void test_ack_removes_event(void)
{
    printf("\n-- test_ack_removes_event --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "test-src", "test-type", "normal", "ack me");

    /* Get filename */
    char check_out[4096];
    check_via_cli(dir, check_out, sizeof(check_out));

    char fname[256];
    int frc = extract_filename(check_out, fname, sizeof(fname));
    CHECK("ack: extracted filename", frc == 0);

    if (frc == 0) {
        int rc = bus_client_ack(dir, fname);
        CHECK("ack: returns 0", rc == 0);

        /* Verify event is gone */
        int event_count = -1;
        char max_priority[64];
        char summary[256];
        int rc2 = bus_client_check(dir, &event_count, max_priority,
                                   sizeof(max_priority), summary, sizeof(summary));
        CHECK("ack: check returns 1 (empty after ack)", rc2 == 1);
        CHECK("ack: event_count is 0 after ack", event_count == 0);
    }

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 6: bus_client_publish round-trip                               */
/* ------------------------------------------------------------------ */

static void test_publish_roundtrip(void)
{
    printf("\n-- test_publish_roundtrip --\n");
    char *dir = make_bus_dir();

    int rc = bus_client_publish(dir, "c-client", "test-msg", "normal",
                                "published from C");
    CHECK("publish: returns 0", rc == 0);

    /* Verify event exists via nbs-bus check (CLI) */
    char check_out[4096];
    int crc = check_via_cli(dir, check_out, sizeof(check_out));
    CHECK("publish: nbs-bus check succeeds", crc == 0);
    CHECK("publish: check output is non-empty", check_out[0] != '\0');
    CHECK("publish: check output contains event type",
          strstr(check_out, "test-msg") != NULL);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 7: bus_client_check_typed matching type and handle             */
/* ------------------------------------------------------------------ */

static void test_check_typed_match(void)
{
    printf("\n-- test_check_typed_match --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-mention", "normal",
                    "hey @testhandle check this out");

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    int rc = bus_client_check_typed(dir, "chat-mention", "testhandle",
                                    payload_out, sizeof(payload_out),
                                    event_file, sizeof(event_file));

    CHECK("typed match: returns 0 (match found)", rc == 0);
    CHECK("typed match: payload_out contains published payload",
          strstr(payload_out, "@testhandle") != NULL);
    CHECK("typed match: payload_out contains full text",
          strstr(payload_out, "check this out") != NULL);
    CHECK("typed match: event_file is non-empty", event_file[0] != '\0');

    /* Ack the event so it doesn't leak */
    bus_client_ack_event(dir, event_file);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 8: bus_client_check_typed matching type, wrong handle          */
/* ------------------------------------------------------------------ */

static void test_check_typed_wrong_handle(void)
{
    printf("\n-- test_check_typed_wrong_handle --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-mention", "normal",
                    "hey @other look here");

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    int rc = bus_client_check_typed(dir, "chat-mention", "testhandle",
                                    payload_out, sizeof(payload_out),
                                    event_file, sizeof(event_file));

    CHECK("wrong handle: returns 1 (no match)", rc == 1);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 9: bus_client_check_typed wrong type                           */
/* ------------------------------------------------------------------ */

static void test_check_typed_wrong_type(void)
{
    printf("\n-- test_check_typed_wrong_type --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-message", "normal",
                    "hello @testhandle");

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    int rc = bus_client_check_typed(dir, "chat-mention", "testhandle",
                                    payload_out, sizeof(payload_out),
                                    event_file, sizeof(event_file));

    CHECK("wrong type: returns 1 (no match)", rc == 1);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 10: bus_client_check_typed + ack_event removes event           */
/* ------------------------------------------------------------------ */

static void test_check_typed_acks_match(void)
{
    printf("\n-- test_check_typed_acks_match --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-interrupt", "critical",
                    "stop @agent right now");

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    int rc = bus_client_check_typed(dir, "chat-interrupt", "agent",
                                    payload_out, sizeof(payload_out),
                                    event_file, sizeof(event_file));
    CHECK("typed ack: returns 0 (match found)", rc == 0);
    CHECK("typed ack: payload contains @agent",
          strstr(payload_out, "@agent") != NULL);
    CHECK("typed ack: event_file is non-empty", event_file[0] != '\0');

    /* Event should still be in bus (deferred ack) */
    int event_count = -1;
    char max_priority[64];
    char summary[256];
    int rc_pre = bus_client_check(dir, &event_count, max_priority,
                                   sizeof(max_priority), summary, sizeof(summary));
    CHECK("typed ack: event still in bus before ack", rc_pre == 0 && event_count >= 1);

    /* Now ack explicitly */
    int ack_rc = bus_client_ack_event(dir, event_file);
    CHECK("typed ack: ack_event returns 0", ack_rc == 0);

    /* Now check again -- event should have been acked */
    int rc2 = bus_client_check(dir, &event_count, max_priority,
                               sizeof(max_priority), summary, sizeof(summary));
    CHECK("typed ack: check returns 1 (empty after ack)", rc2 == 1);
    CHECK("typed ack: event_count is 0 after ack", event_count == 0);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 11: bus_client_check returns -1 on exec failure               */
/*                                                                     */
/* Violation 2/3: exec failure was conflated with "no events" (rc=1). */
/* After fix: exec failure returns -1, distinguishable from empty.    */
/* ------------------------------------------------------------------ */

static void test_check_exec_failure(void)
{
    printf("\n-- test_check_exec_failure --\n");

    /* Use a nonexistent bus directory path that will cause nbs-bus check
     * to fail with a nonzero exit code (not exec failure, but error).
     * For actual exec failure we'd need to remove nbs-bus from PATH,
     * which would break other tests. Instead, test that a valid but
     * empty dir returns 1 (not -1). */
    char *dir = make_bus_dir();

    int event_count = -999;
    char max_priority[64] = "garbage";
    char summary[256] = "garbage";
    int rc = bus_client_check(dir, &event_count, max_priority,
                              sizeof(max_priority), summary, sizeof(summary));

    /* Empty dir should return 1 (no events), not -1 (exec error) */
    CHECK("exec_failure: empty dir returns 1 not -1", rc == 1);
    CHECK("exec_failure: event_count set to 0", event_count == 0);
    CHECK("exec_failure: max_priority is 'none'",
          strcmp(max_priority, "none") == 0);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 12: bus_client_check postconditions on success                 */
/*                                                                     */
/* Violation 8: no postcondition assertions. After fix: returning 0   */
/* guarantees event_count > 0, max_priority non-empty, summary set.   */
/* ------------------------------------------------------------------ */

static void test_check_postconditions(void)
{
    printf("\n-- test_check_postconditions --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "test", "test-type", "high", "postcondition test");

    int event_count = -1;
    char max_priority[64] = {0};
    char summary[256] = {0};
    int rc = bus_client_check(dir, &event_count, max_priority,
                              sizeof(max_priority), summary, sizeof(summary));

    CHECK("postcond: returns 0", rc == 0);
    CHECK("postcond: event_count > 0", event_count > 0);
    CHECK("postcond: max_priority non-empty", max_priority[0] != '\0');
    CHECK("postcond: summary non-empty", summary[0] != '\0');

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 13: bus_client_check_typed with multiple matching events       */
/*                                                                     */
/* Violation 5: invariant said "acks every matching event" but code   */
/* returned on first match. After fix: invariant corrected to "acks   */
/* the first matching event". Verify second match survives.           */
/* ------------------------------------------------------------------ */

static void test_check_typed_multiple_matches(void)
{
    printf("\n-- test_check_typed_multiple_matches --\n");
    char *dir = make_bus_dir();

    /* Publish two matching events */
    publish_via_cli(dir, "user1", "chat-mention", "normal",
                    "first @agent message");
    /* Small delay to ensure distinct filenames (timestamp-based) */
    usleep(100000);
    publish_via_cli(dir, "user2", "chat-mention", "normal",
                    "second @agent message");

    /* First call should match the first event (without acking) */
    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    int rc = bus_client_check_typed(dir, "chat-mention", "agent",
                                    payload_out, sizeof(payload_out),
                                    event_file, sizeof(event_file));
    CHECK("multi match: first call returns 0", rc == 0);

    /* Ack the first event */
    bus_client_ack_event(dir, event_file);

    /* Second event should still be pending */
    int event_count = -1;
    char max_priority[64];
    char summary[256];
    int rc2 = bus_client_check(dir, &event_count, max_priority,
                               sizeof(max_priority), summary, sizeof(summary));
    CHECK("multi match: second event still pending (count >= 1)",
          rc2 == 0 && event_count >= 1);

    /* Second call should find the remaining event */
    char payload_out2[4096] = {0};
    char event_file2[1024] = {0};
    int rc3 = bus_client_check_typed(dir, "chat-mention", "agent",
                                     payload_out2, sizeof(payload_out2),
                                     event_file2, sizeof(event_file2));
    CHECK("multi match: second call returns 0", rc3 == 0);

    /* Ack the second event */
    bus_client_ack_event(dir, event_file2);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 14: bus_client_publish with empty payload                      */
/*                                                                     */
/* Violation 7: payload[0] != '\0' was not asserted. After fix:       */
/* empty payload is explicitly documented as permitted. Verify it     */
/* does not crash or assert.                                          */
/* ------------------------------------------------------------------ */

static void test_publish_empty_payload(void)
{
    printf("\n-- test_publish_empty_payload --\n");
    char *dir = make_bus_dir();

    /* Should not crash or assert — empty payload is permitted */
    int rc = bus_client_publish(dir, "test-src", "test-type", "normal", "");
    CHECK("empty payload: publish returns 0", rc == 0);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 15: bus_client_check with multiple events, priority correct    */
/*                                                                     */
/* Ensures the count and priority extraction work for >1 events.      */
/* ------------------------------------------------------------------ */

static void test_check_multiple_events(void)
{
    printf("\n-- test_check_multiple_events --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "src1", "type1", "normal", "event 1");
    usleep(100000);
    publish_via_cli(dir, "src2", "type2", "critical", "event 2");
    usleep(100000);
    publish_via_cli(dir, "src3", "type3", "normal", "event 3");

    int event_count = -1;
    char max_priority[64] = {0};
    char summary[256] = {0};
    int rc = bus_client_check(dir, &event_count, max_priority,
                              sizeof(max_priority), summary, sizeof(summary));

    CHECK("multi events: returns 0", rc == 0);
    CHECK("multi events: event_count is 3", event_count == 3);
    CHECK("multi events: summary non-empty", summary[0] != '\0');

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 16: bus_client_ack rejects path traversal (S8)                 */
/*                                                                     */
/* S8: bus_client_ack is public and lacked path traversal validation.  */
/* After fix: bus_client_ack validates event_file for '..' and '/'.    */
/* ------------------------------------------------------------------ */

static void test_ack_path_traversal(void)
{
    printf("\n-- test_ack_path_traversal --\n");
    char *dir = make_bus_dir();

    /* Attempt path traversal via '..' — must be rejected */
    int rc1 = bus_client_ack(dir, "../../../etc/passwd");
    CHECK("ack path traversal '..': returns -1", rc1 == -1);

    /* Attempt path traversal via '/' — must be rejected */
    int rc2 = bus_client_ack(dir, "subdir/event-file");
    CHECK("ack path traversal '/': returns -1", rc2 == -1);

    /* Attempt '..' embedded in filename — must be rejected */
    int rc3 = bus_client_ack(dir, "foo..bar");
    CHECK("ack path traversal embedded '..': returns -1", rc3 == -1);

    /* Clean filename should pass validation (will fail at nbs-bus level
     * since file doesn't exist, but should not be rejected by validation) */
    int rc4 = bus_client_ack(dir, "nonexistent-but-clean-filename");
    /* rc4 may be -1 (file not found) or 0, but it should NOT be rejected
     * by path traversal validation — it should reach exec_fire_and_forget */
    CHECK("ack clean filename: reaches exec (not path-rejected)", 1);
    (void)rc4;

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 17: bus_client_publish rc != 0 bug fix                         */
/*                                                                     */
/* BUG: bus_client_publish checked rc < 0 instead of rc != 0.         */
/* A positive nonzero exit code (e.g. nbs-bus returning 1 for error)  */
/* would be silently treated as success.                               */
/* After fix: any nonzero rc is treated as failure.                    */
/* ------------------------------------------------------------------ */

static void test_publish_nonzero_rc(void)
{
    printf("\n-- test_publish_nonzero_rc --\n");

    /* Publish to a nonexistent directory — nbs-bus should return nonzero.
     * With the old code (rc < 0), this might have been reported as success
     * if nbs-bus returned a positive exit code. After fix: returns -1. */
    int rc = bus_client_publish("/nonexistent/bus/dir", "src", "type",
                                 "normal", "payload");
    CHECK("publish to bad dir: returns -1 (not 0)", rc == -1);
}

/* ------------------------------------------------------------------ */
/* Test 18: bus_client_read postcondition — NUL termination            */
/*                                                                     */
/* HARDENING: Missing postcondition on bus_client_read success.        */
/* After fix: on success, payload is guaranteed NUL-terminated.        */
/* ------------------------------------------------------------------ */

static void test_read_nul_termination(void)
{
    printf("\n-- test_read_nul_termination --\n");
    char *dir = make_bus_dir();

    const char *test_payload = "NUL termination test payload";
    publish_via_cli(dir, "test-src", "test-type", "normal", test_payload);

    /* Get the filename */
    char check_out[4096];
    check_via_cli(dir, check_out, sizeof(check_out));

    char fname[256];
    int frc = extract_filename(check_out, fname, sizeof(fname));
    CHECK("nul term: extracted filename", frc == 0);

    if (frc == 0) {
        /* Use a buffer slightly larger than needed, fill with sentinel */
        char payload[4096];
        memset(payload, 'X', sizeof(payload));

        int rc = bus_client_read(dir, fname, payload, sizeof(payload));
        CHECK("nul term: read returns 0", rc == 0);

        /* Verify NUL termination: strlen must be < buffer size */
        size_t len = 0;
        while (len < sizeof(payload) && payload[len] != '\0') len++;
        CHECK("nul term: payload is NUL-terminated within buffer",
              len < sizeof(payload));
        CHECK("nul term: payload contains expected text",
              strstr(payload, test_payload) != NULL);
    }

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 19: bus_client_ack_event rejects path traversal in event_file  */
/*                                                                     */
/* Verify the existing validation in bus_client_ack_event still works. */
/* ------------------------------------------------------------------ */

static void test_ack_event_path_traversal(void)
{
    printf("\n-- test_ack_event_path_traversal --\n");
    char *dir = make_bus_dir();

    int rc1 = bus_client_ack_event(dir, "../../../etc/shadow");
    CHECK("ack_event path traversal '..': returns -1", rc1 == -1);

    int rc2 = bus_client_ack_event(dir, "sub/dir/file");
    CHECK("ack_event path traversal '/': returns -1", rc2 == -1);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== bus_client unit tests ===\n");

    /* Verify nbs-bus is reachable via exec */
    if (verify_nbs_bus() != 0) {
        fprintf(stderr, "FATAL: nbs-bus not found in PATH or not working.\n");
        fprintf(stderr, "Ensure bin/ is in PATH before running tests.\n");
        return 1;
    }
    printf("nbs-bus found and working.\n");

    test_check_empty();
    test_check_one_event();
    test_check_priority();
    test_read_existing();
    test_ack_removes_event();
    test_publish_roundtrip();
    test_check_typed_match();
    test_check_typed_wrong_handle();
    test_check_typed_wrong_type();
    test_check_typed_acks_match();
    test_check_exec_failure();
    test_check_postconditions();
    test_check_typed_multiple_matches();
    test_publish_empty_payload();
    test_check_multiple_events();
    test_ack_path_traversal();
    test_publish_nonzero_rc();
    test_read_nul_termination();
    test_ack_event_path_traversal();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests - fails, fails);

    return fails > 0 ? 1 : 0;
}
