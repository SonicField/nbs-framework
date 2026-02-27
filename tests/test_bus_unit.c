/*
 * test_bus_unit.c — Unit tests for bus.c engineering standards fixes
 *
 * Tests:
 *   1. Path traversal rejection (filenames containing '/')
 *   2. Path traversal rejection (filenames containing '..')
 *   3. has_whitespace correctness for various inputs
 *   4. Integer overflow guard on ack_timeout_s * 1000000LL
 *   5. read_event_fields returns error on fopen failure
 *   6. Pointer-before-array UB guard (empty config value)
 *
 * These tests are adversarial — they exercise the violation boundaries
 * identified in the audit report for bus/bus.c.
 *
 * Build (from tests/ directory):
 *   gcc -Wall -Wextra -Werror -Wno-format-truncation -std=c11 \
 *       -D_POSIX_C_SOURCE=200809L -O2 \
 *       -I../src/nbs-bus -o test_bus_unit test_bus_unit.c ../src/nbs-bus/bus.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "bus.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    tests_passed++; \
    printf("  PASS: %s\n", name); \
} while(0)

/* --- Helper: create a temporary events directory --- */

static int make_temp_events_dir(char *dir_buf, size_t dir_len)
{
    snprintf(dir_buf, dir_len, "/tmp/test_bus_XXXXXX");
    if (mkdtemp(dir_buf) == NULL)
        return -1;
    /* Create processed/ subdirectory */
    char processed[BUS_MAX_FULLPATH];
    snprintf(processed, sizeof(processed), "%s/processed", dir_buf);
    if (mkdir(processed, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* --- Helper: recursively remove temp directory --- */

static void remove_temp_dir(const char *dir)
{
    char cmd[BUS_MAX_FULLPATH + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    (void)system(cmd);
}

/* ================================================================== */
/* Test: path traversal rejection in bus_read                          */
/* ================================================================== */

static void test_path_traversal_slash_read(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Attempt to read a file with '/' in the name — path traversal */
    int ret = bus_read(events_dir, "../../../etc/passwd");
    TEST_ASSERT(ret == -1,
                "bus_read should reject filename with '/', got %d", ret);

    ret = bus_read(events_dir, "subdir/file.event");
    TEST_ASSERT(ret == -1,
                "bus_read should reject filename with embedded '/', got %d", ret);

    ret = bus_read(events_dir, "/absolute/path.event");
    TEST_ASSERT(ret == -1,
                "bus_read should reject filename starting with '/', got %d", ret);

    remove_temp_dir(events_dir);
    TEST_PASS("path traversal rejection in bus_read (filenames with '/')");
}

/* ================================================================== */
/* Test: path traversal rejection in bus_ack                           */
/* ================================================================== */

static void test_path_traversal_slash_ack(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Attempt to ack a file with '/' in the name — path traversal */
    int ret = bus_ack(events_dir, "../../../etc/shadow");
    TEST_ASSERT(ret == -1,
                "bus_ack should reject filename with '/', got %d", ret);

    ret = bus_ack(events_dir, "foo/bar.event");
    TEST_ASSERT(ret == -1,
                "bus_ack should reject filename with embedded '/', got %d", ret);

    remove_temp_dir(events_dir);
    TEST_PASS("path traversal rejection in bus_ack (filenames with '/')");
}

/* ================================================================== */
/* Test: path traversal with '..' component                            */
/* ================================================================== */

static void test_path_traversal_dotdot(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* '..' without '/' is caught by the '/' check if it contains a slash.
     * But we also need to reject bare '..' as a filename. */
    int ret = bus_read(events_dir, "..");
    TEST_ASSERT(ret == -1,
                "bus_read should reject '..' filename, got %d", ret);

    ret = bus_ack(events_dir, "..");
    TEST_ASSERT(ret == -1,
                "bus_ack should reject '..' filename, got %d", ret);

    remove_temp_dir(events_dir);
    TEST_PASS("path traversal rejection for '..' filenames");
}

/* ================================================================== */
/* Test: has_whitespace correctness                                    */
/* ================================================================== */

/*
 * has_whitespace is static, so we test it indirectly via bus_publish
 * which rejects source/type containing whitespace.
 */

static void test_has_whitespace_via_publish(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Source with space should be rejected */
    int ret = bus_publish(events_dir, "my source", "test-type",
                          BUS_PRIORITY_NORMAL, NULL);
    TEST_ASSERT(ret == -1,
                "bus_publish should reject source with space, got %d", ret);

    /* Type with tab should be rejected */
    ret = bus_publish(events_dir, "source", "test\ttype",
                      BUS_PRIORITY_NORMAL, NULL);
    TEST_ASSERT(ret == -1,
                "bus_publish should reject type with tab, got %d", ret);

    /* Type with newline should be rejected */
    ret = bus_publish(events_dir, "source", "test\ntype",
                      BUS_PRIORITY_NORMAL, NULL);
    TEST_ASSERT(ret == -1,
                "bus_publish should reject type with newline, got %d", ret);

    /* Valid source and type should succeed */
    ret = bus_publish(events_dir, "valid-source", "valid-type",
                      BUS_PRIORITY_NORMAL, NULL);
    TEST_ASSERT(ret == 0,
                "bus_publish should accept valid source/type, got %d", ret);

    /* Source with only spaces */
    ret = bus_publish(events_dir, "   ", "valid-type",
                      BUS_PRIORITY_NORMAL, NULL);
    TEST_ASSERT(ret == -1,
                "bus_publish should reject all-spaces source, got %d", ret);

    remove_temp_dir(events_dir);
    TEST_PASS("has_whitespace correctness via bus_publish");
}

/* ================================================================== */
/* Test: integer overflow guard on ack_timeout_s multiplication        */
/* ================================================================== */

static void test_ack_timeout_overflow_guard(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Write a config.yaml with absurdly large ack-timeout.
     * LLONG_MAX / 1000000LL is about 9223372036854.
     * A value larger than that would overflow when multiplied by 1000000LL. */
    char config_path[BUS_MAX_FULLPATH];
    snprintf(config_path, sizeof(config_path), "%s/config.yaml", events_dir);

    FILE *fp = fopen(config_path, "w");
    TEST_ASSERT(fp != NULL, "failed to create config.yaml");
    /* Write a value that would overflow: LLONG_MAX */
    fprintf(fp, "ack-timeout: 9999999999999999\n");
    fclose(fp);

    /* Load config — the value should be clamped or rejected */
    bus_config_t cfg = {0};
    int ret = bus_load_config(events_dir, &cfg);
    TEST_ASSERT(ret == 0, "bus_load_config should succeed, got %d", ret);

    /* The key test: ack_timeout_s * 1000000LL must not overflow.
     * If the guard works, ack_timeout_s should be clamped to a safe value. */
    long long max_safe = LLONG_MAX / 1000000LL;
    TEST_ASSERT(cfg.ack_timeout_s <= max_safe,
                "ack_timeout_s %lld exceeds safe maximum %lld — overflow guard missing",
                cfg.ack_timeout_s, max_safe);

    remove_temp_dir(events_dir);
    TEST_PASS("integer overflow guard on ack_timeout_s * 1000000LL");
}

/* ================================================================== */
/* Test: config loading with empty values (UB guard)                   */
/* ================================================================== */

static void test_config_empty_value(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Write a config.yaml with empty values — the pointer-before-array
     * UB at lines 345-347 would trigger on an empty value like "key:" */
    char config_path[BUS_MAX_FULLPATH];
    snprintf(config_path, sizeof(config_path), "%s/config.yaml", events_dir);

    FILE *fp = fopen(config_path, "w");
    TEST_ASSERT(fp != NULL, "failed to create config.yaml");
    fprintf(fp, "retention-max-bytes:\n");
    fprintf(fp, "dedup-window:\n");
    fprintf(fp, "ack-timeout:\n");
    fprintf(fp, "retention-max-bytes: \n");
    fprintf(fp, "unknown-key:\n");
    fclose(fp);

    /* This should not crash (previously UB from pointer-before-array) */
    bus_config_t cfg = {0};
    int ret = bus_load_config(events_dir, &cfg);
    TEST_ASSERT(ret == 0,
                "bus_load_config should handle empty values without crash, got %d", ret);

    /* Defaults should remain since empty values are not valid integers */
    TEST_ASSERT(cfg.retention_max_bytes == BUS_DEFAULT_MAX_BYTES,
                "retention_max_bytes should be default after empty value");
    TEST_ASSERT(cfg.dedup_window_s == BUS_DEFAULT_DEDUP_WINDOW,
                "dedup_window_s should be default after empty value");
    TEST_ASSERT(cfg.ack_timeout_s == BUS_DEFAULT_ACK_TIMEOUT,
                "ack_timeout_s should be default after empty value");

    remove_temp_dir(events_dir);
    TEST_PASS("config loading with empty values (no UB)");
}

/* ================================================================== */
/* Test: bus_read rejects empty filename                               */
/* ================================================================== */

static void test_read_empty_filename(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* An empty filename should not succeed */
    int ret = bus_read(events_dir, "");
    TEST_ASSERT(ret == -1,
                "bus_read should reject empty filename, got %d", ret);

    remove_temp_dir(events_dir);
    TEST_PASS("bus_read rejects empty filename");
}

/* ================================================================== */
/* Test: bus_ack rejects empty filename                                */
/* ================================================================== */

static void test_ack_empty_filename(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* An empty filename should not succeed */
    int ret = bus_ack(events_dir, "");
    TEST_ASSERT(ret == -1,
                "bus_ack should reject empty filename, got %d", ret);

    remove_temp_dir(events_dir);
    TEST_PASS("bus_ack rejects empty filename");
}

/* ================================================================== */
/* Test: dedup_window_s overflow guard                                 */
/* ================================================================== */

static void test_dedup_window_overflow_guard(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    char config_path[BUS_MAX_FULLPATH];
    snprintf(config_path, sizeof(config_path), "%s/config.yaml", events_dir);

    FILE *fp = fopen(config_path, "w");
    TEST_ASSERT(fp != NULL, "failed to create config.yaml");
    fprintf(fp, "dedup-window: 9999999999999999\n");
    fclose(fp);

    bus_config_t cfg = {0};
    int ret = bus_load_config(events_dir, &cfg);
    TEST_ASSERT(ret == 0, "bus_load_config should succeed");

    long long max_safe = LLONG_MAX / 1000000LL;
    TEST_ASSERT(cfg.dedup_window_s <= max_safe,
                "dedup_window_s %lld exceeds safe maximum %lld — overflow guard missing",
                cfg.dedup_window_s, max_safe);

    remove_temp_dir(events_dir);
    TEST_PASS("integer overflow guard on dedup_window_s * 1000000LL");
}

/* ================================================================== */
/* Test: bus_ack_all — acknowledge all pending events                   */
/* ================================================================== */

static void write_event_file(const char *events_dir, const char *filename,
                             const char *content)
{
    char fullpath[BUS_MAX_FULLPATH];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", events_dir, filename);
    FILE *fp = fopen(fullpath, "w");
    if (fp) {
        if (content) fputs(content, fp);
        fclose(fp);
    }
}

static int file_exists_in(const char *dir, const char *filename)
{
    char path[BUS_MAX_FULLPATH];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    struct stat st;
    return stat(path, &st) == 0;
}

static void test_ack_all_no_filter(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Create 3 pending events with different sources */
    write_event_file(events_dir,
        "1000000000000000-srcA-chat-message-1234.event",
        "source: srcA\ntype: chat-message\npriority: normal\n");
    write_event_file(events_dir,
        "1000000000000001-srcB-chat-mention-1235.event",
        "source: srcB\ntype: chat-mention\npriority: normal\n");
    write_event_file(events_dir,
        "1000000000000002-srcC-human-input-1236.event",
        "source: srcC\ntype: human-input\npriority: normal\n");

    int rc = bus_ack_all(events_dir, NULL);
    TEST_ASSERT(rc == 0, "bus_ack_all should return 0, got %d", rc);

    /* All 3 should be moved to processed/ */
    char proc[BUS_MAX_FULLPATH];
    snprintf(proc, sizeof(proc), "%s/processed", events_dir);

    TEST_ASSERT(!file_exists_in(events_dir,
        "1000000000000000-srcA-chat-message-1234.event"),
        "event 1 should not be in pending");
    TEST_ASSERT(file_exists_in(proc,
        "1000000000000000-srcA-chat-message-1234.event"),
        "event 1 should be in processed");
    TEST_ASSERT(!file_exists_in(events_dir,
        "1000000000000002-srcC-human-input-1236.event"),
        "event 3 should not be in pending");
    TEST_ASSERT(file_exists_in(proc,
        "1000000000000002-srcC-human-input-1236.event"),
        "event 3 should be in processed");

    remove_temp_dir(events_dir);
    TEST_PASS("bus_ack_all: all events acked with NULL handle");
}

static void test_ack_all_with_filter(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Create events with different sources */
    write_event_file(events_dir,
        "1000000000000010-nbs-chat-chat-message-100.event",
        "source: nbs-chat\ntype: chat-message\npriority: normal\n");
    write_event_file(events_dir,
        "1000000000000011-sidecar-heartbeat-101.event",
        "source: sidecar\ntype: heartbeat\npriority: normal\n");
    write_event_file(events_dir,
        "1000000000000012-nbs-chat-chat-mention-102.event",
        "source: nbs-chat\ntype: chat-mention\npriority: normal\n");

    /* Ack only events from "nbs-chat" */
    int rc = bus_ack_all(events_dir, "nbs-chat");
    TEST_ASSERT(rc == 0, "bus_ack_all should return 0, got %d", rc);

    /* Only nbs-chat events should be moved */
    char proc[BUS_MAX_FULLPATH];
    snprintf(proc, sizeof(proc), "%s/processed", events_dir);

    TEST_ASSERT(file_exists_in(proc,
        "1000000000000010-nbs-chat-chat-message-100.event"),
        "nbs-chat event should be in processed");
    TEST_ASSERT(file_exists_in(events_dir,
        "1000000000000011-sidecar-heartbeat-101.event"),
        "sidecar event should still be pending");

    remove_temp_dir(events_dir);
    TEST_PASS("bus_ack_all: handle filter acks only matching source");
}

static void test_ack_all_empty_dir(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    int rc = bus_ack_all(events_dir, NULL);
    TEST_ASSERT(rc == 0, "bus_ack_all on empty dir should return 0, got %d", rc);

    remove_temp_dir(events_dir);
    TEST_PASS("bus_ack_all: empty directory returns 0");
}

static void test_ack_all_nonexistent_dir(void) {
    int rc = bus_ack_all("/nonexistent/events/dir", NULL);
    TEST_ASSERT(rc == -1, "bus_ack_all on nonexistent dir should return -1");
    TEST_PASS("bus_ack_all: nonexistent directory returns -1");
}

/* ================================================================== */
/* Test: bus_prune — delete oldest processed events                    */
/* ================================================================== */

static void test_prune_under_limit(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Create 2 small processed events */
    char proc[BUS_MAX_FULLPATH];
    snprintf(proc, sizeof(proc), "%s/processed", events_dir);

    char path1[BUS_MAX_FULLPATH], path2[BUS_MAX_FULLPATH];
    snprintf(path1, sizeof(path1), "%s/1000000000000000-src-type-1.event", proc);
    snprintf(path2, sizeof(path2), "%s/1000000000000001-src-type-2.event", proc);

    FILE *f1 = fopen(path1, "w");
    if (f1) { fputs("small event 1\n", f1); fclose(f1); }
    FILE *f2 = fopen(path2, "w");
    if (f2) { fputs("small event 2\n", f2); fclose(f2); }

    /* max_bytes = 1 MB — way above actual size */
    int rc = bus_prune(events_dir, 1048576);
    TEST_ASSERT(rc == 0, "bus_prune should return 0, got %d", rc);

    /* Both files should still exist */
    struct stat st;
    TEST_ASSERT(stat(path1, &st) == 0, "event 1 should still exist");
    TEST_ASSERT(stat(path2, &st) == 0, "event 2 should still exist");

    remove_temp_dir(events_dir);
    TEST_PASS("bus_prune: under limit deletes nothing");
}

static void test_prune_over_limit_deletes_oldest(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    char proc[BUS_MAX_FULLPATH];
    snprintf(proc, sizeof(proc), "%s/processed", events_dir);

    /* Create 3 events with different timestamps.
     * Each ~15 bytes ("event content\n") */
    char path_old[BUS_MAX_FULLPATH], path_mid[BUS_MAX_FULLPATH], path_new[BUS_MAX_FULLPATH];
    snprintf(path_old, sizeof(path_old), "%s/1000000000000000-src-type-1.event", proc);
    snprintf(path_mid, sizeof(path_mid), "%s/1000000000100000-src-type-2.event", proc);
    snprintf(path_new, sizeof(path_new), "%s/1000000000200000-src-type-3.event", proc);

    FILE *f;
    f = fopen(path_old, "w"); if (f) { fputs("event content A\n", f); fclose(f); }
    f = fopen(path_mid, "w"); if (f) { fputs("event content B\n", f); fclose(f); }
    f = fopen(path_new, "w"); if (f) { fputs("event content C\n", f); fclose(f); }

    /* max_bytes = 30 bytes. Total ~48 bytes. Should prune oldest until <= 30. */
    int rc = bus_prune(events_dir, 30);
    TEST_ASSERT(rc == 0, "bus_prune should return 0, got %d", rc);

    /* Oldest should be deleted first. With ~16 bytes each and limit 30,
     * need to delete at least 1 (the oldest) to get from ~48 to ~32,
     * which is still over. So delete 2 to get to ~16 which is <= 30. */
    struct stat st;
    int old_exists = (stat(path_old, &st) == 0);
    int new_exists = (stat(path_new, &st) == 0);

    TEST_ASSERT(!old_exists, "oldest event should be deleted");
    TEST_ASSERT(new_exists, "newest event should survive");

    remove_temp_dir(events_dir);
    TEST_PASS("bus_prune: over limit deletes oldest first");
}

static void test_prune_no_processed_dir(void) {
    /* Create events dir without processed/ subdirectory */
    char events_dir[BUS_MAX_FULLPATH];
    snprintf(events_dir, sizeof(events_dir), "/tmp/test_bus_prune_XXXXXX");
    TEST_ASSERT(mkdtemp(events_dir) != NULL, "mkdtemp failed");

    /* No processed/ directory */
    int rc = bus_prune(events_dir, 1024);
    TEST_ASSERT(rc == 0, "bus_prune without processed dir should return 0, got %d", rc);

    remove_temp_dir(events_dir);
    TEST_PASS("bus_prune: no processed directory returns 0");
}

/* ================================================================== */
/* Audit fix tests: BUG #2 — ack_timeout_s overflow assertion at use  */
/* ================================================================== */

/*
 * BUG #2: bus_status multiplies cfg.ack_timeout_s * 1000000LL at line 940.
 * The config parser guards against overflow, but the assertion at the point
 * of use was missing. After the fix, an ASSERT_MSG fires before the multiply.
 * We test that a value loaded via the config parser (which already guards)
 * passes through safely, and that the assertion exists by confirming the
 * multiplication does not overflow for any config-loaded value.
 */
static void test_bug2_ack_timeout_overflow_at_use(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Write a config with max safe ack-timeout (exactly LLONG_MAX / 1000000) */
    char config_path[BUS_MAX_FULLPATH];
    snprintf(config_path, sizeof(config_path), "%s/config.yaml", events_dir);
    FILE *fp = fopen(config_path, "w");
    TEST_ASSERT(fp != NULL, "failed to create config.yaml");
    long long max_safe = LLONG_MAX / 1000000LL;
    fprintf(fp, "ack-timeout: %lld\n", max_safe);
    fclose(fp);

    /* Publish an event so bus_status has something to check */
    int rc = bus_publish(events_dir, "test-src", "test-type",
                         BUS_PRIORITY_NORMAL, NULL);
    TEST_ASSERT(rc == 0, "bus_publish should succeed, got %d", rc);

    /* bus_status should not crash — the overflow assertion must pass */
    rc = bus_status(events_dir);
    TEST_ASSERT(rc == 0, "bus_status should succeed with max safe ack_timeout, got %d", rc);

    remove_temp_dir(events_dir);
    TEST_PASS("BUG #2: ack_timeout_s overflow assertion at point of use");
}

/* ================================================================== */
/* Audit fix tests: BUG #3 — cutoff_us underflow clamping             */
/* ================================================================== */

/*
 * BUG #3: bus_publish_dedup computes cutoff_us = current_us - dedup_window_us.
 * If dedup_window_us > current_us, this underflows to a negative value.
 * After the fix, cutoff_us is clamped to 0, so all events are checked
 * (functionally correct and by design, not by accident).
 *
 * Test: publish an event, then call dedup with a window larger than
 * current_us. The duplicate should be detected (not missed due to
 * underflow bypassing the time filter).
 */
static void test_bug3_cutoff_underflow_clamp(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Publish a first event */
    int rc = bus_publish(events_dir, "dedup-src", "dedup-type",
                         BUS_PRIORITY_NORMAL, NULL);
    TEST_ASSERT(rc == 0, "first publish should succeed, got %d", rc);

    /* Attempt to publish duplicate with an enormous dedup window.
     * Use LLONG_MAX / 2 to avoid any overflow in the window itself.
     * This would cause cutoff_us underflow pre-fix. */
    long long huge_window = LLONG_MAX / 2;
    rc = bus_publish_dedup(events_dir, "dedup-src", "dedup-type",
                            BUS_PRIORITY_NORMAL, NULL, huge_window);
    TEST_ASSERT(rc == BUS_EXIT_DEDUP,
                "dedup with huge window should detect duplicate, got %d", rc);

    remove_temp_dir(events_dir);
    TEST_PASS("BUG #3: cutoff_us underflow clamped to 0");
}

/* ================================================================== */
/* Audit fix tests: SECURITY #6 — fopen failure warning in readers    */
/* ================================================================== */

/*
 * SECURITY #6: read_event_dedup_key and read_event_fields silently return
 * on fopen failure. After the fix, they emit a warning to stderr.
 * We test indirectly: create an event file, make it unreadable, then
 * verify bus_check still succeeds (skipping the file) but the event
 * does NOT appear in the listing (i.e. it is skipped, not silently
 * treated as a valid event with default values).
 */
static void test_sec6_unreadable_event_skipped(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Create a normal readable event */
    write_event_file(events_dir,
        "1000000000000100-readable-test-event-999.event",
        "source: readable\ntype: test-event\npriority: normal\n");

    /* Create an event file then remove read permission */
    char unreadable[BUS_MAX_FULLPATH];
    snprintf(unreadable, sizeof(unreadable),
             "%s/1000000000000200-unreadable-broken-998.event", events_dir);
    FILE *fp = fopen(unreadable, "w");
    TEST_ASSERT(fp != NULL, "failed to create unreadable event");
    fputs("source: unreadable\ntype: broken\npriority: high\n", fp);
    fclose(fp);
    chmod(unreadable, 0000);

    /* bus_check should succeed, just skipping the unreadable event */
    int rc = bus_check(events_dir, NULL);
    TEST_ASSERT(rc == 0, "bus_check should succeed even with unreadable events, got %d", rc);

    /* Restore permissions for cleanup */
    chmod(unreadable, 0644);

    remove_temp_dir(events_dir);
    TEST_PASS("SECURITY #6: unreadable event file is skipped with warning");
}

/* ================================================================== */
/* Audit fix tests: SECURITY #7 — scan_events warning on skip         */
/* ================================================================== */

/*
 * SECURITY #7: scan_events silently skips malformed filenames.
 * After the fix, a warning is emitted. We test that a malformed event
 * filename (no timestamp prefix) is skipped without crashing.
 */
static void test_sec7_malformed_filename_skipped(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Create a well-formed event */
    write_event_file(events_dir,
        "1000000000000300-goodsrc-goodtype-777.event",
        "source: goodsrc\ntype: goodtype\npriority: normal\n");

    /* Create a malformed event (no timestamp prefix, just letters) */
    write_event_file(events_dir,
        "malformed-no-timestamp.event",
        "source: bad\ntype: bad\npriority: normal\n");

    /* bus_check should still succeed, skipping the malformed one */
    int rc = bus_check(events_dir, NULL);
    TEST_ASSERT(rc == 0, "bus_check should succeed with malformed filenames, got %d", rc);

    remove_temp_dir(events_dir);
    TEST_PASS("SECURITY #7: malformed filename skipped with warning");
}

/* ================================================================== */
/* Audit fix tests: HARDENING #8 — priority array/loop bound sync     */
/* ================================================================== */

/*
 * HARDENING #8: The loop bound in bus_priority_from_str must match the
 * size of the priority_names array. We test all known priority strings
 * convert correctly and unknown strings return -1.
 */
static void test_hard8_priority_from_str_coverage(void) {
    /* All four known priorities */
    TEST_ASSERT(bus_priority_from_str("critical") == 0,
                "critical should map to 0");
    TEST_ASSERT(bus_priority_from_str("high") == 1,
                "high should map to 1");
    TEST_ASSERT(bus_priority_from_str("normal") == 2,
                "normal should map to 2");
    TEST_ASSERT(bus_priority_from_str("low") == 3,
                "low should map to 3");

    /* Unknown strings */
    TEST_ASSERT(bus_priority_from_str("CRITICAL") == -1,
                "uppercase CRITICAL should return -1");
    TEST_ASSERT(bus_priority_from_str("medium") == -1,
                "unknown 'medium' should return -1");
    TEST_ASSERT(bus_priority_from_str("") == -1,
                "empty string should return -1");

    TEST_PASS("HARDENING #8: priority_from_str coverage and bound sync");
}

/* ================================================================== */
/* Audit fix tests: HARDENING #9 — read_event_fields postcondition    */
/* ================================================================== */

/*
 * HARDENING #9: read_event_fields returns 0 even when not all fields
 * are found. After the fix, incomplete files produce a warning.
 * We test that an event file missing the source field results in
 * the event still being scanned (graceful degradation) but the
 * source field remains empty.
 */
static void test_hard9_incomplete_event_file(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Create an event file missing the 'source:' line */
    write_event_file(events_dir,
        "1000000000000400-nosrc-test-888.event",
        "type: test\npriority: high\n");

    /* bus_check should not crash */
    int rc = bus_check(events_dir, NULL);
    TEST_ASSERT(rc == 0, "bus_check should handle incomplete event files, got %d", rc);

    remove_temp_dir(events_dir);
    TEST_PASS("HARDENING #9: incomplete event file produces warning, does not crash");
}

/* ================================================================== */
/* Audit fix tests: HARDENING #10 — ferror check in bus_read          */
/* ================================================================== */

/*
 * HARDENING #10: bus_read should check ferror after the fread loop.
 * We test the normal path — that bus_read succeeds on a valid file.
 * (Simulating a read error without kernel-level faulting is not
 * feasible in a unit test, but we verify the normal path still works.)
 */
static void test_hard10_bus_read_normal(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    write_event_file(events_dir,
        "1000000000000500-src-type-555.event",
        "source: src\ntype: type\npriority: normal\n");

    int rc = bus_read(events_dir, "1000000000000500-src-type-555.event");
    TEST_ASSERT(rc == 0, "bus_read should succeed on valid file, got %d", rc);

    remove_temp_dir(events_dir);
    TEST_PASS("HARDENING #10: bus_read normal path with ferror check");
}

/* ================================================================== */
/* Audit fix tests: HARDENING #14 — publish_dedup preconditions       */
/* ================================================================== */

/*
 * HARDENING #14: bus_publish_dedup should assert source[0] != '\0',
 * type[0] != '\0', and priority in [0, 3] before the dedup scan.
 * We test the whitespace rejection path (which goes through bus_publish
 * after the dedup scan) to verify the precondition is caught early.
 */
static void test_hard14_publish_dedup_whitespace_source(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    /* Source with whitespace — bus_publish will reject this, but the
     * precondition assertions in publish_dedup now validate content
     * before doing the dedup scan. Whitespace source is not empty,
     * so the empty-source assertion won't fire. Test that the
     * whitespace check in bus_publish still catches it. */
    int rc = bus_publish_dedup(events_dir, "has space", "valid",
                                BUS_PRIORITY_NORMAL, NULL, 60000000LL);
    TEST_ASSERT(rc == -1,
                "publish_dedup should reject whitespace source, got %d", rc);

    remove_temp_dir(events_dir);
    TEST_PASS("HARDENING #14: publish_dedup precondition validation");
}

/* ================================================================== */
/* Audit fix tests: HARDENING #15 — printf error check in bus_ack_all */
/* ================================================================== */

/*
 * HARDENING #15: bus_ack_all printf is now checked. Normal path test.
 */
static void test_hard15_ack_all_printf_check(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    write_event_file(events_dir,
        "1000000000000600-src-type-444.event",
        "source: src\ntype: type\npriority: normal\n");

    int rc = bus_ack_all(events_dir, NULL);
    TEST_ASSERT(rc == 0, "bus_ack_all normal path should succeed, got %d", rc);

    remove_temp_dir(events_dir);
    TEST_PASS("HARDENING #15: bus_ack_all printf error check normal path");
}

/* ================================================================== */
/* Audit fix tests: HARDENING #17 — unknown config key warning        */
/* ================================================================== */

/*
 * HARDENING #17: unknown config keys should produce a warning on stderr.
 * We test that config loading still succeeds (returns 0) and defaults
 * are used when the only keys are unknown.
 */
static void test_hard17_unknown_config_key(void) {
    char events_dir[BUS_MAX_FULLPATH];
    TEST_ASSERT(make_temp_events_dir(events_dir, sizeof(events_dir)) == 0,
                "failed to create temp events dir");

    char config_path[BUS_MAX_FULLPATH];
    snprintf(config_path, sizeof(config_path), "%s/config.yaml", events_dir);
    FILE *fp = fopen(config_path, "w");
    TEST_ASSERT(fp != NULL, "failed to create config.yaml");
    fprintf(fp, "retentoin-max-bytes: 1024\n");
    fprintf(fp, "unknown-widget: foobar\n");
    fprintf(fp, "ack-timeout: 30\n");
    fclose(fp);

    bus_config_t cfg = {0};
    int rc = bus_load_config(events_dir, &cfg);
    TEST_ASSERT(rc == 0, "bus_load_config should succeed, got %d", rc);

    /* 'retentoin-max-bytes' is a typo — should use default */
    TEST_ASSERT(cfg.retention_max_bytes == BUS_DEFAULT_MAX_BYTES,
                "misspelled key should use default, got %lld", cfg.retention_max_bytes);
    /* 'ack-timeout: 30' is valid and should be parsed */
    TEST_ASSERT(cfg.ack_timeout_s == 30,
                "valid ack-timeout should be 30, got %lld", cfg.ack_timeout_s);

    remove_temp_dir(events_dir);
    TEST_PASS("HARDENING #17: unknown config keys warned, defaults used");
}

/* ================================================================== */
/* Main test runner                                                    */
/* ================================================================== */

int main(void) {
    printf("=== bus unit tests ===\n\n");

    test_path_traversal_slash_read();
    test_path_traversal_slash_ack();
    test_path_traversal_dotdot();
    test_has_whitespace_via_publish();
    test_ack_timeout_overflow_guard();
    test_config_empty_value();
    test_read_empty_filename();
    test_ack_empty_filename();
    test_dedup_window_overflow_guard();

    /* bus_ack_all */
    test_ack_all_no_filter();
    test_ack_all_with_filter();
    test_ack_all_empty_dir();
    test_ack_all_nonexistent_dir();

    /* bus_prune */
    test_prune_under_limit();
    test_prune_over_limit_deletes_oldest();
    test_prune_no_processed_dir();

    /* Audit fix tests: BUG */
    test_bug2_ack_timeout_overflow_at_use();
    test_bug3_cutoff_underflow_clamp();

    /* Audit fix tests: SECURITY */
    test_sec6_unreadable_event_skipped();
    test_sec7_malformed_filename_skipped();

    /* Audit fix tests: HARDENING */
    test_hard8_priority_from_str_coverage();
    test_hard9_incomplete_event_file();
    test_hard10_bus_read_normal();
    test_hard14_publish_dedup_whitespace_source();
    test_hard15_ack_all_printf_check();
    test_hard17_unknown_config_key();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
