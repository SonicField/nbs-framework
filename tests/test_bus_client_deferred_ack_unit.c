/*
 * test_bus_client_deferred_ack_unit.c -- Unit tests for deferred ack API.
 *
 * Tests bus_client_check_typed (updated to NOT ack on match) and
 * the new bus_client_ack_event function.
 *
 * Falsification approach:
 *   - Tests 1-4 verify the deferred ack contract: check_typed returns
 *     event data WITHOUT acking; ack_event moves to processed/.
 *     These tests would PASS on old code only if they didn't check
 *     event persistence — the explicit "still in bus" check falsifies.
 *   - Tests 5-8 verify boundary conditions and error paths.
 *   - Tests 9-12 verify atomicity, idempotency, and truncation.
 *
 * Requires: nbs-bus binary in PATH.
 *
 * Build (from Makefile):
 *   make -C src/nbs-sidecar test-unit
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

static char *make_bus_dir(void)
{
    char tmpl[] = "/tmp/nbs-bus-deferred-XXXXXX";
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

static int count_events(const char *bus_dir)
{
    int event_count = 0;
    char max_priority[64];
    char summary[256];
    int rc = bus_client_check(bus_dir, &event_count, max_priority,
                              sizeof(max_priority), summary, sizeof(summary));
    if (rc != 0) return 0;
    return event_count;
}

static int verify_nbs_bus(void)
{
    char *dir = make_bus_dir();
    int rc = publish_via_cli(dir, "probe", "probe", "normal", "probe");
    rmdir_recursive(dir);
    free(dir);
    return (rc == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Test 1: check_typed returns event_file and payload without acking   */
/* ------------------------------------------------------------------ */

static void test_check_typed_no_ack(void)
{
    printf("\n-- test 1: check_typed returns event_file and payload without acking --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-query", "high",
                    "@agent? what are you doing");

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    int rc = bus_client_check_typed(dir, "chat-query", "agent",
                                    payload_out, sizeof(payload_out),
                                    event_file, sizeof(event_file));

    CHECK("returns 0 (match found)", rc == 0);
    CHECK("payload_out contains @agent",
          strstr(payload_out, "@agent") != NULL);
    CHECK("event_file is non-empty", event_file[0] != '\0');

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 2: Event still in bus after check_typed returns                 */
/* ------------------------------------------------------------------ */

static void test_event_persists_after_check(void)
{
    printf("\n-- test 2: event still in bus after check_typed returns --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-query", "high",
                    "@agent? status please");

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    int rc = bus_client_check_typed(dir, "chat-query", "agent",
                                    payload_out, sizeof(payload_out),
                                    event_file, sizeof(event_file));
    CHECK("check_typed returns 0", rc == 0);

    /* Event must still be in bus — this is the key falsification */
    int remaining = count_events(dir);
    CHECK("event still in bus (count >= 1)", remaining >= 1);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 3: ack_event moves event to processed/                         */
/* ------------------------------------------------------------------ */

static void test_ack_event_moves(void)
{
    printf("\n-- test 3: ack_event moves event to processed/ --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-query", "high",
                    "@agent? show me");

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    bus_client_check_typed(dir, "chat-query", "agent",
                           payload_out, sizeof(payload_out),
                           event_file, sizeof(event_file));

    int rc = bus_client_ack_event(dir, event_file);
    CHECK("ack_event returns 0", rc == 0);

    /* Verify event is gone from bus */
    int remaining = count_events(dir);
    CHECK("event gone from bus after ack", remaining == 0);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 4: Event gone from bus after ack_event                         */
/* ------------------------------------------------------------------ */

static void test_event_gone_after_ack(void)
{
    printf("\n-- test 4: second check_typed finds nothing after ack --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-query", "high",
                    "@agent? check");

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    bus_client_check_typed(dir, "chat-query", "agent",
                           payload_out, sizeof(payload_out),
                           event_file, sizeof(event_file));
    bus_client_ack_event(dir, event_file);

    /* Second check should find nothing */
    char payload_out2[4096] = {0};
    char event_file2[1024] = {0};
    int rc = bus_client_check_typed(dir, "chat-query", "agent",
                                    payload_out2, sizeof(payload_out2),
                                    event_file2, sizeof(event_file2));
    CHECK("second check returns 1 (no match)", rc == 1);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 5: check_typed with no matching events returns 1               */
/* ------------------------------------------------------------------ */

static void test_check_typed_no_match(void)
{
    printf("\n-- test 5: check_typed with no matching events returns 1 --\n");
    char *dir = make_bus_dir();

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    int rc = bus_client_check_typed(dir, "chat-query", "agent",
                                    payload_out, sizeof(payload_out),
                                    event_file, sizeof(event_file));
    CHECK("empty bus returns 1", rc == 1);
    CHECK("event_file stays empty", event_file[0] == '\0');

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 6: check_typed with wrong handle returns 1, event stays        */
/* ------------------------------------------------------------------ */

static void test_check_typed_wrong_handle_persists(void)
{
    printf("\n-- test 6: check_typed with wrong handle returns 1, event stays --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-query", "high",
                    "@alice? what up");

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    int rc = bus_client_check_typed(dir, "chat-query", "bob",
                                    payload_out, sizeof(payload_out),
                                    event_file, sizeof(event_file));
    CHECK("wrong handle returns 1", rc == 1);

    /* Event must still be in bus for the correct handle's sidecar */
    int remaining = count_events(dir);
    CHECK("event stays for correct handle (count >= 1)", remaining >= 1);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 7: ack_event on already-acked event returns -1 (ENOENT)        */
/* ------------------------------------------------------------------ */

static void test_ack_already_acked(void)
{
    printf("\n-- test 7: ack_event on already-acked event returns -1 --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-query", "high",
                    "@agent? test");

    char payload_out[4096] = {0};
    char event_file[1024] = {0};
    bus_client_check_typed(dir, "chat-query", "agent",
                           payload_out, sizeof(payload_out),
                           event_file, sizeof(event_file));

    /* First ack succeeds */
    int rc1 = bus_client_ack_event(dir, event_file);
    CHECK("first ack returns 0", rc1 == 0);

    /* Second ack should fail — event already moved */
    int rc2 = bus_client_ack_event(dir, event_file);
    CHECK("second ack returns -1 (already acked)", rc2 == -1);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 8: ack_event on nonexistent event returns -1                   */
/* ------------------------------------------------------------------ */

static void test_ack_nonexistent(void)
{
    printf("\n-- test 8: ack_event on nonexistent event returns -1 --\n");
    char *dir = make_bus_dir();

    int rc = bus_client_ack_event(dir, "nonexistent-event-file.evt");
    CHECK("ack nonexistent returns -1", rc == -1);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 9: check_typed + ack_event is atomic (no event loss)           */
/* ------------------------------------------------------------------ */

static void test_check_ack_atomic(void)
{
    printf("\n-- test 9: check_typed + ack_event round-trip is complete --\n");
    char *dir = make_bus_dir();

    /* Publish 3 events, process them one at a time */
    publish_via_cli(dir, "u1", "chat-query", "high", "@agent? q1");
    usleep(100000);
    publish_via_cli(dir, "u2", "chat-query", "high", "@agent? q2");
    usleep(100000);
    publish_via_cli(dir, "u3", "chat-query", "high", "@agent? q3");

    int processed = 0;
    for (int i = 0; i < 5; i++) {  /* 5 iterations, expect 3 matches */
        char payload_out[4096] = {0};
        char event_file[1024] = {0};
        int rc = bus_client_check_typed(dir, "chat-query", "agent",
                                        payload_out, sizeof(payload_out),
                                        event_file, sizeof(event_file));
        if (rc == 0) {
            bus_client_ack_event(dir, event_file);
            processed++;
        }
    }

    CHECK("all 3 events processed", processed == 3);

    /* Bus should now be empty */
    int remaining = count_events(dir);
    CHECK("bus empty after processing all", remaining == 0);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 10: Multiple check_typed calls return same event (idempotent)  */
/* ------------------------------------------------------------------ */

static void test_check_idempotent_before_ack(void)
{
    printf("\n-- test 10: multiple check_typed calls return same event --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-query", "high",
                    "@agent? idempotent test");

    char payload1[4096] = {0}, payload2[4096] = {0};
    char event_file1[1024] = {0}, event_file2[1024] = {0};

    int rc1 = bus_client_check_typed(dir, "chat-query", "agent",
                                     payload1, sizeof(payload1),
                                     event_file1, sizeof(event_file1));
    int rc2 = bus_client_check_typed(dir, "chat-query", "agent",
                                     payload2, sizeof(payload2),
                                     event_file2, sizeof(event_file2));

    CHECK("first call returns 0", rc1 == 0);
    CHECK("second call returns 0", rc2 == 0);
    CHECK("both return same event file",
          strcmp(event_file1, event_file2) == 0);
    CHECK("both return same payload",
          strcmp(payload1, payload2) == 0);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 11: ack_event with invalid (path-traversal) filename rejected  */
/* ------------------------------------------------------------------ */

static void test_ack_invalid_filename(void)
{
    printf("\n-- test 11: ack_event with invalid filename rejected --\n");
    char *dir = make_bus_dir();

    /* Path traversal attempt — must not escape bus dir */
    int rc = bus_client_ack_event(dir, "../../../etc/passwd");
    CHECK("path traversal rejected (returns -1)", rc == -1);

    /* Slash in filename */
    int rc2 = bus_client_ack_event(dir, "sub/dir/file");
    CHECK("slash in filename rejected (returns -1)", rc2 == -1);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Test 12: check_typed with truncated buffer warns and continues      */
/* ------------------------------------------------------------------ */

static void test_check_typed_small_event_file_buffer(void)
{
    printf("\n-- test 12: check_typed with small event_file buffer --\n");
    char *dir = make_bus_dir();

    publish_via_cli(dir, "user1", "chat-query", "high",
                    "@agent? buffer test");

    char payload_out[4096] = {0};
    /* Use a very small event_file buffer — too small for the filename */
    char event_file[4] = {0};
    int rc = bus_client_check_typed(dir, "chat-query", "agent",
                                    payload_out, sizeof(payload_out),
                                    event_file, sizeof(event_file));

    /* Should return the payload but indicate event_file couldn't be stored.
     * The function should still return 0 (match found) and set payload,
     * but event_file will be empty (couldn't fit), meaning caller can't ack. */
    CHECK("returns 0 (match found despite small buffer)", rc == 0);
    CHECK("payload still populated",
          strstr(payload_out, "@agent") != NULL);

    rmdir_recursive(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== bus_client deferred ack unit tests ===\n");

    if (verify_nbs_bus() != 0) {
        fprintf(stderr, "FATAL: nbs-bus not found in PATH or not working.\n");
        fprintf(stderr, "Ensure bin/ is in PATH before running tests.\n");
        return 1;
    }
    printf("nbs-bus found and working.\n");

    test_check_typed_no_ack();
    test_event_persists_after_check();
    test_ack_event_moves();
    test_event_gone_after_ack();
    test_check_typed_no_match();
    test_check_typed_wrong_handle_persists();
    test_ack_already_acked();
    test_ack_nonexistent();
    test_check_ack_atomic();
    test_check_idempotent_before_ack();
    test_ack_invalid_filename();
    test_check_typed_small_event_file_buffer();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests - fails, fails);

    return fails > 0 ? 1 : 0;
}
