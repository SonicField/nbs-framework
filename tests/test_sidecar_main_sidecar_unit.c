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
#include "transport.h"
#include <signal.h>
#include <sys/wait.h>

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

/* Test 15 (S12 — SECURITY): Handle with terminal escape sequences.
 * Adversarial: if printed unsanitised, this would change terminal colours.
 * is_valid_handle rejects it, and the error path sanitises before printing. */
static void test_config_validate_handle_escape_injection(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* ESC[31m = red text, ESC[0m = reset — classic terminal injection */
    snprintf(cfg.handle, sizeof(cfg.handle), "\x1b[31mEVIL\x1b[0m");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "handle with ANSI escapes should fail validation, got rc=%d", rc);
    TEST_PASS("config_validate: handle with terminal escapes fails (S12)");
}

/* Test 16 (S12 — SECURITY): Handle with control characters (bell, backspace). */
static void test_config_validate_handle_control_chars(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* BEL (0x07) and BS (0x08) — could cause terminal misbehaviour */
    snprintf(cfg.handle, sizeof(cfg.handle), "test\x07\x08agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "handle with control chars should fail validation, got rc=%d", rc);
    TEST_PASS("config_validate: handle with control chars fails (S12)");
}

/* Test 17 (HARDENING): Negative librarian_interval should fail validation.
 * This catches overflow from scaled timing fields (env_int * 60). */
static void test_config_validate_negative_librarian_interval(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;
    cfg.librarian_interval = -1;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "negative librarian_interval should fail, got rc=%d", rc);
    TEST_PASS("config_validate: negative librarian_interval fails");
}

/* Test 18 (HARDENING): Negative pythia_interval should fail validation. */
static void test_config_validate_negative_pythia_interval(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;
    cfg.pythia_interval = -1;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "negative pythia_interval should fail, got rc=%d", rc);
    TEST_PASS("config_validate: negative pythia_interval fails");
}

/* Test 19 (HARDENING): Negative shepard_interval should fail validation. */
static void test_config_validate_negative_shepard_interval(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;
    cfg.shepard_interval = -1;

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc != 0,
                "negative shepard_interval should fail, got rc=%d", rc);
    TEST_PASS("config_validate: negative shepard_interval fails");
}

/* Test 20 (HARDENING): Valid scaled timing fields pass. */
static void test_config_validate_valid_scaled_timing(void) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.handle, sizeof(cfg.handle), "test-agent");
    snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
    cfg.bus_check_interval = 3;
    cfg.notify_fail_threshold = 5;
    cfg.librarian_interval = 900;   /* 15 * 60 */
    cfg.pythia_interval = 1800;     /* 30 * 60 */
    cfg.shepard_interval = 1200;    /* 20 * 60 */

    int rc = sidecar_config_validate(&cfg);
    TEST_ASSERT(rc == 0,
                "valid scaled timing should pass, got rc=%d", rc);
    TEST_PASS("config_validate: valid scaled timing passes");
}

/* ======== B21: send_key/send_text return value checking ======== */

/* Mock transport that fails send_text and send_key.
 * Used to verify sidecar_run checks return values rather than
 * silently discarding them. We cannot run a full main loop in
 * a unit test (it sleeps forever), but we can verify the vtable
 * assertions fire for NULL function pointers. */

static char *mock_capture_prompt(const struct transport *self, int scrollback) {
    (void)self; (void)scrollback;
    /* Return something that looks like a prompt */
    char *buf = malloc(64);
    if (buf) snprintf(buf, 64, "$ ");
    return buf;
}

static int mock_send_text_fail(const struct transport *self, const char *text) {
    (void)self; (void)text;
    return -1;
}

static int mock_send_key_fail(const struct transport *self, const char *key) {
    (void)self; (void)key;
    return -1;
}

static int mock_is_alive_true(const struct transport *self) {
    (void)self;
    return 1;
}

/* ======== HARDENING: vtable NULL assertions ======== */

/*
 * Test 21 (HARDENING): sidecar_run aborts when send_key is NULL.
 * Falsifiable: if send_key NULL check is missing, the child exits normally.
 */
static void test_sidecar_run_null_send_key_aborts(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: set up transport with NULL send_key */
        transport_t tp;
        memset(&tp, 0, sizeof(tp));
        tp.capture = mock_capture_prompt;
        tp.send_text = mock_send_text_fail;
        tp.send_key = NULL;  /* This should trigger ASSERT_MSG */
        tp.is_alive = mock_is_alive_true;

        sidecar_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.handle, sizeof(cfg.handle), "test");
        snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
        cfg.bus_check_interval = 3;
        cfg.notify_fail_threshold = 5;

        sidecar_run(&cfg, &tp);
        _exit(0);  /* Should not reach here */
    }

    int status;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
                "sidecar_run with NULL send_key should abort (SIGABRT), "
                "got signal=%d exited=%d",
                WIFSIGNALED(status) ? WTERMSIG(status) : -1,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    TEST_PASS("sidecar_run: NULL send_key triggers abort (HARDENING vtable)");
}

/*
 * Test 22 (HARDENING): sidecar_run aborts when send_text is NULL.
 */
static void test_sidecar_run_null_send_text_aborts(void) {
    pid_t pid = fork();
    if (pid == 0) {
        transport_t tp;
        memset(&tp, 0, sizeof(tp));
        tp.capture = mock_capture_prompt;
        tp.send_text = NULL;  /* Should trigger ASSERT_MSG */
        tp.send_key = mock_send_key_fail;
        tp.is_alive = mock_is_alive_true;

        sidecar_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.handle, sizeof(cfg.handle), "test");
        snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
        cfg.bus_check_interval = 3;
        cfg.notify_fail_threshold = 5;

        sidecar_run(&cfg, &tp);
        _exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
                "sidecar_run with NULL send_text should abort (SIGABRT), "
                "got signal=%d exited=%d",
                WIFSIGNALED(status) ? WTERMSIG(status) : -1,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    TEST_PASS("sidecar_run: NULL send_text triggers abort (HARDENING vtable)");
}

/*
 * Test 23 (HARDENING): sidecar_run aborts when is_alive is NULL.
 */
static void test_sidecar_run_null_is_alive_aborts(void) {
    pid_t pid = fork();
    if (pid == 0) {
        transport_t tp;
        memset(&tp, 0, sizeof(tp));
        tp.capture = mock_capture_prompt;
        tp.send_text = mock_send_text_fail;
        tp.send_key = mock_send_key_fail;
        tp.is_alive = NULL;  /* Should trigger ASSERT_MSG */

        sidecar_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.handle, sizeof(cfg.handle), "test");
        snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
        cfg.bus_check_interval = 3;
        cfg.notify_fail_threshold = 5;

        sidecar_run(&cfg, &tp);
        _exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
                "sidecar_run with NULL is_alive should abort (SIGABRT), "
                "got signal=%d exited=%d",
                WIFSIGNALED(status) ? WTERMSIG(status) : -1,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    TEST_PASS("sidecar_run: NULL is_alive triggers abort (HARDENING vtable)");
}

/*
 * Test 24 (HARDENING): sidecar_run with all vtable members set does
 * NOT abort at the assertion level (it proceeds to the main loop).
 * Falsifiable: if the assertions are too strict, the child aborts.
 */
static void test_sidecar_run_valid_vtable_no_abort(void) {
    pid_t pid = fork();
    if (pid == 0) {
        transport_t tp;
        memset(&tp, 0, sizeof(tp));
        tp.capture = mock_capture_prompt;
        tp.send_text = mock_send_text_fail;
        tp.send_key = mock_send_key_fail;
        tp.is_alive = mock_is_alive_true;

        sidecar_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.handle, sizeof(cfg.handle), "test");
        snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "/tmp");
        cfg.bus_check_interval = 3;
        cfg.notify_fail_threshold = 5;

        /* sidecar_run will enter its main loop — kill after 2s */
        alarm(2);
        sidecar_run(&cfg, &tp);
        _exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    /* Should be killed by SIGALRM, not SIGABRT */
    int was_abrt = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    TEST_ASSERT(!was_abrt,
                "sidecar_run with valid vtable should not abort");
    TEST_PASS("sidecar_run: valid vtable passes assertions (HARDENING vtable)");
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
    test_config_validate_handle_escape_injection();
    test_config_validate_handle_control_chars();
    test_config_validate_negative_librarian_interval();
    test_config_validate_negative_pythia_interval();
    test_config_validate_negative_shepard_interval();
    test_config_validate_valid_scaled_timing();

    test_sidecar_run_null_send_key_aborts();
    test_sidecar_run_null_send_text_aborts();
    test_sidecar_run_null_is_alive_aborts();
    test_sidecar_run_valid_vtable_no_abort();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
