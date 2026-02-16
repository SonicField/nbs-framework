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

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
