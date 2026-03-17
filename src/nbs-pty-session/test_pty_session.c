/*
 * test_pty_session.c — Adversarial tests for pty-session main.c and session.c.
 *
 * Tests target BUG, SECURITY, and HARDENING violations from the audit report.
 * Each test is named after the violation it exercises.
 *
 * The approach: we test the internal helper functions (parse_int_option,
 * join_args, is_safe_name, is_safe_home_path, sanitise_for_display) and
 * the cmd_* functions for boundary conditions.
 *
 * Build: gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L
 *        -D_DEFAULT_SOURCE -DTEST_BUILD -I../nbs-common
 *        -o test_pty_session test_pty_session.c session.c main_testable.c
 *
 * We compile with -DTEST_BUILD so main.c can expose its static functions
 * for testing. The actual exposure is done via test-visible wrappers.
 */

#include "session.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Test infrastructure ─────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST: %-60s ", #name); \
    fflush(stdout); \
} while(0)

#define PASS() do { \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define FAIL(msg, ...) do { \
    tests_failed++; \
    printf("FAIL\n"); \
    fprintf(stderr, "    DETAIL: " msg "\n", ##__VA_ARGS__); \
} while(0)

/*
 * We need access to static functions from main.c for testing.
 * The test-visible wrappers are declared in session.h when TEST_BUILD
 * is defined, and implemented in main.c.
 */

/* ── Tests for main.c violations ─────────────────────────────────── */

/*
 * Violation M1 (BUG): parse_int_option silently returns default on
 * invalid input. After fix, it should return -1 on parse failure.
 */
static void test_parse_int_option_valid(void)
{
    TEST(parse_int_option_valid_value);
    int result = test_parse_int_option("--timeout=42", 10);
    if (result == 42) {
        PASS();
    } else {
        FAIL("Expected 42, got %d", result);
    }
}

static void test_parse_int_option_garbage(void)
{
    TEST(parse_int_option_garbage_returns_error);
    int result = test_parse_int_option("--timeout=banana", 10);
    if (result == -1) {
        PASS();
    } else {
        FAIL("Expected -1 (error sentinel) for garbage input, got %d", result);
    }
}

static void test_parse_int_option_zero(void)
{
    TEST(parse_int_option_zero_returns_error);
    int result = test_parse_int_option("--timeout=0", 10);
    if (result == -1) {
        PASS();
    } else {
        FAIL("Expected -1 (error sentinel) for zero, got %d", result);
    }
}

static void test_parse_int_option_negative(void)
{
    TEST(parse_int_option_negative_returns_error);
    int result = test_parse_int_option("--timeout=-5", 10);
    if (result == -1) {
        PASS();
    } else {
        FAIL("Expected -1 (error sentinel) for negative, got %d", result);
    }
}

static void test_parse_int_option_overflow(void)
{
    TEST(parse_int_option_overflow_returns_error);
    /* Violation M7: strtol overflow must be detected via errno */
    int result = test_parse_int_option("--timeout=99999999999999999999", 10);
    if (result == -1) {
        PASS();
    } else {
        FAIL("Expected -1 (error sentinel) for overflow, got %d", result);
    }
}

static void test_parse_int_option_above_max(void)
{
    TEST(parse_int_option_above_max_returns_error);
    int result = test_parse_int_option("--timeout=100001", 10);
    if (result == -1) {
        PASS();
    } else {
        FAIL("Expected -1 (error sentinel) for value > MAX_OPTION_VALUE, got %d", result);
    }
}

static void test_parse_int_option_at_max(void)
{
    TEST(parse_int_option_at_max_boundary);
    int result = test_parse_int_option("--timeout=100000", 10);
    if (result == 100000) {
        PASS();
    } else {
        FAIL("Expected 100000 at max boundary, got %d", result);
    }
}

static void test_parse_int_option_no_equals(void)
{
    TEST(parse_int_option_no_equals_returns_default);
    int result = test_parse_int_option("--timeout", 10);
    if (result == 10) {
        PASS();
    } else {
        FAIL("Expected default 10 for no equals, got %d", result);
    }
}

static void test_parse_int_option_empty_after_equals(void)
{
    TEST(parse_int_option_empty_after_equals_returns_default);
    int result = test_parse_int_option("--timeout=", 10);
    if (result == 10) {
        PASS();
    } else {
        FAIL("Expected default 10 for empty after equals, got %d", result);
    }
}

static void test_parse_int_option_trailing_chars(void)
{
    TEST(parse_int_option_trailing_chars_returns_error);
    int result = test_parse_int_option("--timeout=42abc", 10);
    if (result == -1) {
        PASS();
    } else {
        FAIL("Expected -1 (error sentinel) for trailing chars, got %d", result);
    }
}

/*
 * Violation M5 (SECURITY): Unsanitised user input in error messages.
 * Test that sanitise_for_display strips non-printable characters.
 */
static void test_sanitise_for_display_normal(void)
{
    TEST(sanitise_for_display_normal_ascii);
    char buf[64];
    test_sanitise_for_display("hello-world_123", buf, sizeof(buf));
    if (strcmp(buf, "hello-world_123") == 0) {
        PASS();
    } else {
        FAIL("Expected 'hello-world_123', got '%s'", buf);
    }
}

static void test_sanitise_for_display_escape_sequences(void)
{
    TEST(sanitise_for_display_strips_ansi_escapes);
    char buf[64];
    /* ANSI clear screen: ESC[2J */
    test_sanitise_for_display("\x1b[2Jhello", buf, sizeof(buf));
    /* Non-printable chars should be replaced with '?' */
    int has_escape = 0;
    for (size_t i = 0; i < strlen(buf); i++) {
        if (buf[i] == '\x1b') {
            has_escape = 1;
            break;
        }
    }
    if (!has_escape && strstr(buf, "hello") != NULL) {
        PASS();
    } else {
        FAIL("Expected escape chars stripped, got '%s'", buf);
    }
}

static void test_sanitise_for_display_truncation(void)
{
    TEST(sanitise_for_display_truncates_long_input);
    char buf[16];
    test_sanitise_for_display("this-is-a-very-long-string-indeed", buf, sizeof(buf));
    if (strlen(buf) < 16 && buf[strlen(buf)] == '\0') {
        PASS();
    } else {
        FAIL("Expected truncation within buffer, got len=%zu", strlen(buf));
    }
}

static void test_sanitise_for_display_null_bytes(void)
{
    TEST(sanitise_for_display_handles_embedded_nulls);
    char buf[64];
    /* String with embedded null - strlen stops at first null, so this
     * effectively tests a short string. The key invariant is no crash. */
    test_sanitise_for_display("ab\0cd", buf, sizeof(buf));
    if (strlen(buf) == 2) {  /* stops at \0 */
        PASS();
    } else {
        FAIL("Expected len 2 (stops at null), got %zu", strlen(buf));
    }
}

/*
 * Tests for is_safe_name (session.c) — validating session name input.
 */
static void test_is_safe_name_valid(void)
{
    TEST(is_safe_name_valid_names);
    const char *valid[] = {"myrepl", "test-session", "build_123", "A", "a-b-c_1-2-3"};
    int ok = 1;
    for (size_t i = 0; i < sizeof(valid)/sizeof(valid[0]); i++) {
        if (test_is_safe_name(valid[i]) != 1) {
            FAIL("valid name rejected: '%s' (index %zu)", valid[i], i);
            ok = 0;
            break;
        }
    }
    if (ok) {
        PASS();
    }
}

static void test_is_safe_name_injection(void)
{
    TEST(is_safe_name_rejects_injection_chars);
    const char *bad[] = {
        "", "foo;bar", "foo|bar", "foo&bar", "foo`bar", "foo$bar",
        "foo'bar", "../etc/passwd", "foo bar", "foo\tbar", "foo\nbar",
        "\x1b[2J"
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        if (test_is_safe_name(bad[i]) != 0) {
            FAIL("injection input not rejected at index %zu: '%s'", i, bad[i]);
            ok = 0;
            break;
        }
    }
    if (ok) {
        PASS();
    }
}

/*
 * Violation S1 (SECURITY): $HOME not validated for shell metacharacters.
 * Test is_safe_home_path.
 */
static void test_is_safe_home_path_normal(void)
{
    TEST(is_safe_home_path_accepts_normal_paths);
    const char *valid[] = {
        "/home/user", "/Users/alex", "/root",
        "/home/user-name_123", "/home/user.name"
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(valid)/sizeof(valid[0]); i++) {
        if (test_is_safe_home_path(valid[i]) != 1) {
            FAIL("valid path rejected: '%s' (index %zu)", valid[i], i);
            ok = 0;
            break;
        }
    }
    if (ok) {
        PASS();
    }
}

static void test_is_safe_home_path_injection(void)
{
    TEST(is_safe_home_path_rejects_shell_injection);
    const char *bad[] = {
        "/tmp/x'; rm -rf /", "/tmp/x`whoami`", "/tmp/$HOME",
        "/tmp/x;id", "/tmp/x|cat", "/tmp/x&bg", ""
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        if (test_is_safe_home_path(bad[i]) != 0) {
            FAIL("injection path not rejected at index %zu: '%s'", i, bad[i]);
            ok = 0;
            break;
        }
    }
    if (ok) {
        PASS();
    }
}

/*
 * Violation S9 (SECURITY): Cache files created world-readable.
 * This is a code-level fix; the test verifies via the create_file_secure
 * helper if available. We test indirectly by checking that the function
 * exists and can be called.
 */

/*
 * Violation S3 (BUG): read() error silently swallowed in exec_capture.
 * NOT TESTED — requires mock of read() syscall; cannot be exercised
 * without fault injection infrastructure.
 */

/*
 * Violation S4 (BUG): fputs return unchecked in cmd_read.
 * NOT TESTED — requires mock of fputs() to force write failure.
 */

/*
 * Violation S5 (BUG): exec_capture non-zero exit treated as success in read_log.
 * NOT TESTED — requires a controlled subprocess that exits non-zero
 * while producing partial output.
 */

/*
 * Violation S6 (BUG): Timeout measured by accumulated sleep, not wall clock.
 * NOT TESTED — requires wall-clock vs accumulated-sleep timing
 * comparison under load; not feasible in unit tests.
 */

/*
 * Violation S7 (HARDENING): No overflow postcondition on timeout * 1000.
 * NOT TESTED — the assertion in the code guards this at runtime;
 * triggering it in a test would abort the test process.
 */

/*
 * Violation S8 (HARDENING): Duplicated log-search block.
 * NOT TESTED — this is a refactoring (structural, not behavioural).
 * Functional coverage comes from cmd_wait tests.
 */

/*
 * Violation S13 (BUG): ferror() not checked after fgets loop.
 * NOT TESTED — requires mock of fgets() to inject I/O error
 * mid-stream.
 */

/*
 * Violation M3/M4 (BUG): dispatch_read/dispatch_wait silently ignore
 * unrecognised options.
 * We test this via dispatching with bad args.
 */
static void test_dispatch_read_rejects_unknown_option(void)
{
    TEST(dispatch_read_rejects_unknown_option);
    /* Construct argv for: pty-session read mysession --typo=5 */
    const char *argv[] = {"pty-session", "read", "testsession", "--typo=5", NULL};
    int rc = test_dispatch_read(4, (char **)argv);
    if (rc == EXIT_BAD_ARGS) {
        PASS();
    } else {
        FAIL("Expected EXIT_BAD_ARGS (4) for unknown option, got %d", rc);
    }
}

static void test_dispatch_wait_rejects_unknown_option(void)
{
    TEST(dispatch_wait_rejects_unknown_option);
    const char *argv[] = {"pty-session", "wait", "testsession", "pattern", "--bogus=1", NULL};
    int rc = test_dispatch_wait(5, (char **)argv);
    if (rc == EXIT_BAD_ARGS) {
        PASS();
    } else {
        FAIL("Expected EXIT_BAD_ARGS (4) for unknown option, got %d", rc);
    }
}

static void test_dispatch_read_accepts_valid_options(void)
{
    TEST(dispatch_read_accepts_valid_options);
    /* These should NOT return EXIT_BAD_ARGS (they may fail for other
     * reasons like no tmux session, but not due to option parsing) */
    const char *argv1[] = {"pty-session", "read", "testsession", "--scrollback=50", NULL};
    int rc1 = test_dispatch_read(4, (char **)argv1);

    const char *argv2[] = {"pty-session", "read", "testsession", "--wait", NULL};
    int rc2 = test_dispatch_read(4, (char **)argv2);

    const char *argv3[] = {"pty-session", "read", "testsession", "--timeout=30", NULL};
    int rc3 = test_dispatch_read(4, (char **)argv3);

    const char *argv4[] = {"pty-session", "read", "testsession", "--last=20", NULL};
    int rc4 = test_dispatch_read(4, (char **)argv4);

    /* Log actual return codes so failures are diagnosable */
    printf("[rc: %d, %d, %d, %d] ", rc1, rc2, rc3, rc4);

    if (rc1 != EXIT_BAD_ARGS && rc2 != EXIT_BAD_ARGS &&
        rc3 != EXIT_BAD_ARGS && rc4 != EXIT_BAD_ARGS) {
        PASS();
    } else {
        FAIL("Valid options incorrectly rejected: "
             "--scrollback=50 rc=%d, --wait rc=%d, --timeout=30 rc=%d, --last=20 rc=%d",
             rc1, rc2, rc3, rc4);
    }
}

static void test_dispatch_read_bad_parse_returns_error(void)
{
    TEST(dispatch_read_bad_option_value_returns_bad_args);
    /* Violation M1: parse failure should propagate EXIT_BAD_ARGS */
    const char *argv[] = {"pty-session", "read", "testsession", "--scrollback=banana", NULL};
    int rc = test_dispatch_read(4, (char **)argv);
    if (rc == EXIT_BAD_ARGS) {
        PASS();
    } else {
        FAIL("Expected EXIT_BAD_ARGS (4) for bad parse, got %d", rc);
    }
}

/* ── Adversarial tests for session.c/session.h audit violations ──── */

/*
 * B4 (BUG): O_RDONLY is 0; (flags & O_RDONLY) always false.
 * After fix: open_secure with O_RDONLY must select "r" mode.
 * Falsification: create a file, open it read-only via open_secure,
 * verify we can read but not write.
 */
static void test_open_secure_rdonly_mode(void)
{
    TEST(B4_open_secure_rdonly_selects_read_mode);

    /* Create a temp file with known content */
    char tmppath[] = "/tmp/nbs_test_b4_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) {
        FAIL("mkstemp failed: %s", strerror(errno));
        return;
    }
    const char *content = "test_content\n";
    ssize_t wr = write(fd, content, strlen(content));
    close(fd);
    if (wr < 0) {
        unlink(tmppath);
        FAIL("write to temp file failed");
        return;
    }

    /* Open read-only via open_secure */
    FILE *f = test_open_secure(tmppath, O_RDONLY);
    if (!f) {
        unlink(tmppath);
        FAIL("open_secure O_RDONLY returned NULL");
        return;
    }

    /* Verify we can read the content */
    char buf[64];
    char *got = fgets(buf, sizeof(buf), f);
    fclose(f);
    unlink(tmppath);

    if (got && strcmp(buf, content) == 0) {
        PASS();
    } else {
        FAIL("open_secure O_RDONLY did not read correctly, got: %s",
             got ? buf : "(null)");
    }
}

/*
 * B4 (BUG): Verify open_secure with O_WRONLY selects "w" mode.
 * Falsification: open for write, write, re-read, verify content.
 */
static void test_open_secure_wronly_mode(void)
{
    TEST(B4_open_secure_wronly_selects_write_mode);

    char tmppath[] = "/tmp/nbs_test_b4w_XXXXXX";
    int fd = mkstemp(tmppath);
    close(fd);

    FILE *f = test_open_secure(tmppath, O_WRONLY | O_TRUNC);
    if (!f) {
        unlink(tmppath);
        FAIL("open_secure O_WRONLY returned NULL");
        return;
    }
    fprintf(f, "written\n");
    fclose(f);

    /* Re-read and verify */
    FILE *rf = fopen(tmppath, "r");
    char buf[64];
    char *got = rf ? fgets(buf, sizeof(buf), rf) : NULL;
    if (rf) fclose(rf);
    unlink(tmppath);

    if (got && strcmp(buf, "written\n") == 0) {
        PASS();
    } else {
        FAIL("open_secure O_WRONLY did not write correctly");
    }
}

/*
 * S9 (SECURITY): Fence file must be created with 0600 permissions.
 * Falsification: create via open_secure, check mode bits.
 */
static void test_open_secure_permissions(void)
{
    TEST(S9_open_secure_creates_file_with_0600);

    char tmppath[] = "/tmp/nbs_test_s9_XXXXXX";
    /* Remove the file mkstemp creates so open_secure creates it fresh */
    int fd = mkstemp(tmppath);
    close(fd);
    unlink(tmppath);

    /* Set permissive umask to prove open_secure enforces 0600 */
    mode_t old_umask = umask(0000);

    FILE *f = test_open_secure(tmppath, O_WRONLY | O_CREAT | O_TRUNC);
    umask(old_umask);

    if (!f) {
        FAIL("open_secure O_WRONLY|O_CREAT returned NULL");
        return;
    }
    fprintf(f, "secret\n");
    fclose(f);

    struct stat st;
    int rc = stat(tmppath, &st);
    unlink(tmppath);

    if (rc != 0) {
        FAIL("stat failed on created file");
        return;
    }

    mode_t perms = st.st_mode & 0777;
    if (perms == 0600) {
        PASS();
    } else {
        FAIL("Expected 0600, got %04o", perms);
    }
}

/*
 * Hardening: get_monotonic_ms returns int64_t, not long.
 * Falsification: verify the value is positive and plausible
 * (> 0, fits in int64_t range).
 */
static void test_get_monotonic_ms_returns_positive(void)
{
    TEST(get_monotonic_ms_returns_positive_int64);
    int64_t ms = test_get_monotonic_ms();
    if (ms > 0) {
        PASS();
    } else {
        FAIL("Expected positive int64_t, got %" PRId64, ms);
    }
}

/*
 * Hardening (session.h): PTY_PREFIX_LEN verified against PTY_PREFIX.
 * Falsification: the _Static_assert in session.h would fail at compile
 * time if they disagree. This test verifies at runtime too.
 */
static void test_pty_prefix_len_matches(void)
{
    TEST(pty_prefix_len_matches_pty_prefix);
    size_t actual = strlen(PTY_PREFIX);
    if (actual == PTY_PREFIX_LEN) {
        PASS();
    } else {
        FAIL("PTY_PREFIX_LEN=%d but strlen(PTY_PREFIX)=%zu",
             PTY_PREFIX_LEN, actual);
    }
}

/*
 * Hardening (session.h): EXIT_SUCCESS_CODE == EXIT_SUCCESS.
 * Falsification: the _Static_assert in session.h would fail at compile
 * time if they disagree. Runtime check for belt-and-braces.
 */
static void test_exit_success_code_matches(void)
{
    TEST(exit_success_code_equals_exit_success);
    if (EXIT_SUCCESS_CODE == EXIT_SUCCESS) {
        PASS();
    } else {
        FAIL("EXIT_SUCCESS_CODE=%d != EXIT_SUCCESS=%d",
             EXIT_SUCCESS_CODE, EXIT_SUCCESS);
    }
}

/* ── Test runner ──────────────────────────────────────────────────── */

int main(void)
{
    printf("=== pty-session adversarial tests ===\n\n");

    printf("[main.c violations]\n");
    /* M1/M7: parse_int_option */
    test_parse_int_option_valid();
    test_parse_int_option_garbage();
    test_parse_int_option_zero();
    test_parse_int_option_negative();
    test_parse_int_option_overflow();
    test_parse_int_option_above_max();
    test_parse_int_option_at_max();
    test_parse_int_option_no_equals();
    test_parse_int_option_empty_after_equals();
    test_parse_int_option_trailing_chars();

    /* M5/M6: sanitise_for_display */
    test_sanitise_for_display_normal();
    test_sanitise_for_display_escape_sequences();
    test_sanitise_for_display_truncation();
    test_sanitise_for_display_null_bytes();

    /* M3/M4: unknown option rejection */
    test_dispatch_read_rejects_unknown_option();
    test_dispatch_wait_rejects_unknown_option();
    test_dispatch_read_accepts_valid_options();
    test_dispatch_read_bad_parse_returns_error();

    printf("\n[session.c violations]\n");
    /* S1: is_safe_name */
    test_is_safe_name_valid();
    test_is_safe_name_injection();

    /* S1: is_safe_home_path */
    test_is_safe_home_path_normal();
    test_is_safe_home_path_injection();

    /* B4: open_secure O_RDONLY fix */
    test_open_secure_rdonly_mode();
    test_open_secure_wronly_mode();

    /* S9: fence file permissions */
    test_open_secure_permissions();

    /* Hardening: get_monotonic_ms int64_t */
    test_get_monotonic_ms_returns_positive();

    /* Hardening: session.h compile-time checks (runtime verification) */
    test_pty_prefix_len_matches();
    test_exit_success_code_matches();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    /* Postcondition: every test must be accounted for as pass or fail */
    ASSERT_MSG(tests_run == tests_passed + tests_failed,
               "test counter invariant: run=%d != passed=%d + failed=%d",
               tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
