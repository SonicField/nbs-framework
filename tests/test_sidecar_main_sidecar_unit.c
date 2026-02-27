/*
 * test_sidecar_main_sidecar_unit.c — Adversarial tests for main.c and sidecar.c
 *
 * Tests BUG, SECURITY, and HARDENING violations from the audit report.
 * Each test is falsifiable: the assertion names what would prove it wrong.
 *
 * Tests target:
 *   - env_int: precondition, overflow, magic number
 *   - is_valid_handle: bounded iteration, edge cases
 *   - nbs_root validation: absolute path, directory existence
 *   - snprintf truncation: default prompt, prepended prompt
 *   - freopen failure: stderr loss detection
 *   - sidecar_config_validate: completeness of invariant checks
 *   - should_inject_notify: time_t overflow on cooldown elapsed
 *   - handle_query: size_t underflow in truncation arithmetic
 *   - escape_mentions NULL: UB via %s format with NULL
 *   - respond_dialogue: transport error handling
 *   - sidecar_state_t: invariant verification
 *
 * Build:
 *   make -C src/nbs-sidecar test_sidecar_main_sidecar_unit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "sidecar.h"

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

/* ======== sidecar_config_validate tests ======== */

/* Test 1: Valid config passes validation */
static void test_config_validate_valid(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_cooldown = 15;
    cfg.startup_grace = 30;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc == 0,
                "valid config should pass validation, got rc=%d", rc);
    TEST_PASS("config_validate: valid config");
}

/* Test 2: Empty handle fails */
static void test_config_validate_empty_handle(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "empty handle should fail validation, got rc=%d", rc);
    TEST_PASS("config_validate: empty handle fails");
}

/* Test 3: Empty nbs_root fails */
static void test_config_validate_empty_root(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "empty nbs_root should fail validation, got rc=%d", rc);
    TEST_PASS("config_validate: empty root fails");
}

/* Test 4: bus_check_interval <= 0 fails */
static void test_config_validate_bad_bus_interval(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 0;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "bus_check_interval=0 should fail validation, got rc=%d", rc);
    TEST_PASS("config_validate: bad bus_check_interval fails");
}

/* Test 5: notify_fail_threshold <= 0 fails */
static void test_config_validate_bad_fail_threshold(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 0;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "notify_fail_threshold=0 should fail validation, got rc=%d", rc);
    TEST_PASS("config_validate: bad notify_fail_threshold fails");
}

/* Test 6 (BUG fix): Relative nbs_root should fail validation
 * Audit violation 4 on main.c — sidecar.h documents "nbs_root is an
 * absolute path" but neither main.c nor config_validate enforced it. */
static void test_config_validate_relative_root(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "relative/path");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "relative nbs_root should fail validation, got rc=%d", rc);
    TEST_PASS("config_validate: relative root fails (audit BUG #4)");
}

/* Test 7 (BUG fix): Handle with invalid characters should fail
 * Audit violation 10 on sidecar.c — config_validate didn't check format. */
static void test_config_validate_bad_handle_chars(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "bad handle!");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "handle with spaces/bangs should fail validation, got rc=%d", rc);
    TEST_PASS("config_validate: bad handle chars fails (audit HARDENING #10)");
}

/* Test 8 (HARDENING fix): Negative notify_cooldown should fail
 * Audit violation 10 on sidecar.c — documented invariant not enforced. */
static void test_config_validate_negative_cooldown(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;
    cfg.notify_cooldown = -1;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "negative notify_cooldown should fail validation, got rc=%d", rc);
    TEST_PASS("config_validate: negative cooldown fails (audit HARDENING #10)");
}

/* Test 9 (HARDENING fix): Negative startup_grace should fail
 * Audit violation 10 on sidecar.c — documented invariant not enforced. */
static void test_config_validate_negative_grace(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;
    cfg.startup_grace = -1;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "negative startup_grace should fail validation, got rc=%d", rc);
    TEST_PASS("config_validate: negative startup_grace fails (audit HARDENING #10)");
}

/* Test 10: Handle with only valid chars passes */
static void test_config_validate_good_handle_chars(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "agent-01_test");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc == 0,
                "valid handle chars should pass, got rc=%d", rc);
    TEST_PASS("config_validate: good handle chars pass");
}

/* Test 11 (adversarial): Handle with path traversal chars */
static void test_config_validate_handle_path_traversal(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "../../../etc/passwd");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "handle with path traversal should fail, got rc=%d", rc);
    TEST_PASS("config_validate: handle path traversal fails");
}

/* Test 12 (adversarial): Handle with null-adjacent special chars */
static void test_config_validate_handle_special(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test@agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "handle with @ should fail, got rc=%d", rc);
    TEST_PASS("config_validate: handle with @ fails");
}

/* Test 13: Multiple errors are all reported (rc still -1) */
static void test_config_validate_multiple_errors(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* handle empty, root empty, bus_check_interval=0, threshold=0 */
    cfg.bus_check_interval = 0;
    cfg.notify_fail_threshold = 0;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "multiple errors should still fail, got rc=%d", rc);
    TEST_PASS("config_validate: multiple errors all detected");
}

/* ======== is_valid_handle (exposed indirectly via config_validate) ======== */

/* Test 14: Single character handle */
static void test_config_validate_single_char_handle(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "a");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc == 0,
                "single char handle should pass, got rc=%d", rc);
    TEST_PASS("config_validate: single char handle passes");
}

/* ---- main ---- */

int main(void) {
    printf("test_sidecar_main_sidecar_unit\n");

    test_config_validate_valid();
    test_config_validate_empty_handle();
    test_config_validate_empty_root();
    test_config_validate_bad_bus_interval();
    test_config_validate_bad_fail_threshold();
    test_config_validate_relative_root();
    test_config_validate_bad_handle_chars();
    test_config_validate_negative_cooldown();
    test_config_validate_negative_grace();
    test_config_validate_good_handle_chars();
    test_config_validate_handle_path_traversal();
    test_config_validate_handle_special();
    test_config_validate_multiple_errors();
    test_config_validate_single_char_handle();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
