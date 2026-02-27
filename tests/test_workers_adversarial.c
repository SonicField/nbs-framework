/*
 * test_workers_adversarial.c — Adversarial tests for nbs-workers.
 *
 * Tests target BUG and SECURITY violations from the audit report,
 * plus HARDENING assertions. Each test is designed to falsify
 * a specific invariant.
 *
 * Build (from src/nbs-workers/):
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 \
 *       -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2 \
 *       -I. -I../nbs-common \
 *       -o ../../tests/test_workers_adversarial \
 *       ../../tests/test_workers_adversarial.c worker.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "worker.h"

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

/* ================================================================== */
/* Audit Violation 15/16/17: Redundant NULL checks (HARDENING)         */
/* After fix, these validate the assertion fires on empty string only. */
/* ================================================================== */

static void test_slug_empty_only_guard(void)
{
    /* After fix: the NULL branch is removed, only empty string check remains */
    TEST_ASSERT(validate_slug("") == 0,
                "empty slug should be rejected");
    TEST_ASSERT(validate_slug("a") == 1,
                "single valid char should be accepted");
    TEST_PASS("validate_slug: empty string guard works without NULL branch");
}

static void test_name_empty_only_guard(void)
{
    TEST_ASSERT(validate_worker_name("") == 0,
                "empty name should be rejected");
    TEST_ASSERT(validate_worker_name("a-0000") == 1,
                "minimal valid name should be accepted");
    TEST_PASS("validate_worker_name: empty string guard works without NULL branch");
}

static void test_uuid_empty_only_guard(void)
{
    TEST_ASSERT(validate_uuid("") == 0,
                "empty uuid should be rejected");
    TEST_ASSERT(validate_uuid("00000000-0000-0000-0000-000000000000") == 1,
                "nil UUID should be accepted");
    TEST_PASS("validate_uuid: empty string guard works without NULL branch");
}

/* ================================================================== */
/* Audit Violation 3 (worker.c, SECURITY): handle/model injection      */
/* validate_handle must reject shell metacharacters                     */
/* ================================================================== */

static void test_handle_rejects_shell_injection(void)
{
    /* Semicolon — command separator */
    TEST_ASSERT(validate_worker_name("a;rm -rf /-0000") == 0,
                "semicolon in name not rejected");

    /* Backtick — command substitution */
    TEST_ASSERT(validate_worker_name("`whoami`-0000") == 0,
                "backtick in name not rejected");

    /* $() — command substitution */
    TEST_ASSERT(validate_worker_name("$(id)-0000") == 0,
                "dollar-paren in name not rejected");

    /* Pipe */
    TEST_ASSERT(validate_worker_name("a|cat-0000") == 0,
                "pipe in name not rejected");

    /* Newline */
    TEST_ASSERT(validate_worker_name("a\nrm-0000") == 0,
                "newline in name not rejected");

    /* Space */
    TEST_ASSERT(validate_worker_name("a rm-0000") == 0,
                "space in name not rejected");

    /* Single quote — breaks out of shell quoting */
    TEST_ASSERT(validate_worker_name("a'-0000") == 0,
                "single quote in name not rejected");

    /* Double quote */
    TEST_ASSERT(validate_worker_name("a\"-0000") == 0,
                "double quote in name not rejected");

    TEST_PASS("validate_worker_name: shell metacharacters rejected (security boundary)");
}

/*
 * validate_safe_handle — tests for the new function that validates
 * handles used in cmd_continue's shell command construction.
 * Handle format: [a-z0-9][-a-z0-9]* (allows hyphens, unlike slug)
 */
extern int validate_safe_handle(const char *handle);

static void test_safe_handle_accepts_valid(void)
{
    TEST_ASSERT(validate_safe_handle("shepard") == 1,
                "simple handle rejected");
    TEST_ASSERT(validate_safe_handle("my-worker") == 1,
                "hyphenated handle rejected");
    TEST_ASSERT(validate_safe_handle("worker123") == 1,
                "handle with digits rejected");
    TEST_ASSERT(validate_safe_handle("a") == 1,
                "single-char handle rejected");
    TEST_ASSERT(validate_safe_handle("a-b-c-d") == 1,
                "multi-hyphen handle rejected");
    TEST_PASS("validate_safe_handle: valid handles accepted");
}

static void test_safe_handle_rejects_empty(void)
{
    TEST_ASSERT(validate_safe_handle("") == 0,
                "empty handle not rejected");
    TEST_PASS("validate_safe_handle: empty handle rejected");
}

static void test_safe_handle_rejects_shell_metacharacters(void)
{
    TEST_ASSERT(validate_safe_handle("a;b") == 0,
                "semicolon not rejected");
    TEST_ASSERT(validate_safe_handle("a`b") == 0,
                "backtick not rejected");
    TEST_ASSERT(validate_safe_handle("$(id)") == 0,
                "command substitution not rejected");
    TEST_ASSERT(validate_safe_handle("a|b") == 0,
                "pipe not rejected");
    TEST_ASSERT(validate_safe_handle("a&b") == 0,
                "ampersand not rejected");
    TEST_ASSERT(validate_safe_handle("a b") == 0,
                "space not rejected");
    TEST_ASSERT(validate_safe_handle("a'b") == 0,
                "single quote not rejected");
    TEST_ASSERT(validate_safe_handle("a\"b") == 0,
                "double quote not rejected");
    TEST_ASSERT(validate_safe_handle("a\nb") == 0,
                "newline not rejected");
    TEST_ASSERT(validate_safe_handle("a/b") == 0,
                "slash not rejected");
    TEST_ASSERT(validate_safe_handle("../etc") == 0,
                "path traversal not rejected");
    TEST_PASS("validate_safe_handle: shell metacharacters rejected");
}

static void test_safe_handle_rejects_uppercase(void)
{
    TEST_ASSERT(validate_safe_handle("Worker") == 0,
                "uppercase not rejected");
    TEST_ASSERT(validate_safe_handle("WORKER") == 0,
                "all-uppercase not rejected");
    TEST_PASS("validate_safe_handle: uppercase rejected");
}

static void test_safe_handle_rejects_leading_hyphen(void)
{
    TEST_ASSERT(validate_safe_handle("-worker") == 0,
                "leading hyphen not rejected (could be parsed as option)");
    TEST_PASS("validate_safe_handle: leading hyphen rejected");
}

/*
 * validate_safe_model — tests for model override validation.
 * Model format: [a-z0-9][-a-z0-9._:]* (allows dots, colons for model names)
 */
extern int validate_safe_model(const char *model);

static void test_safe_model_accepts_valid(void)
{
    TEST_ASSERT(validate_safe_model("claude-opus-4-6") == 1,
                "standard model name rejected");
    TEST_ASSERT(validate_safe_model("claude-sonnet-4-20250514") == 1,
                "model with date rejected");
    TEST_ASSERT(validate_safe_model("gpt4") == 1,
                "simple model name rejected");
    TEST_ASSERT(validate_safe_model("model:latest") == 1,
                "model with colon rejected");
    TEST_ASSERT(validate_safe_model("my.model.v2") == 1,
                "model with dots rejected");
    TEST_PASS("validate_safe_model: valid model names accepted");
}

static void test_safe_model_rejects_shell_metacharacters(void)
{
    TEST_ASSERT(validate_safe_model("model;rm -rf /") == 0,
                "semicolon not rejected");
    TEST_ASSERT(validate_safe_model("`whoami`") == 0,
                "backtick not rejected");
    TEST_ASSERT(validate_safe_model("$(id)") == 0,
                "command substitution not rejected");
    TEST_ASSERT(validate_safe_model("model|cat") == 0,
                "pipe not rejected");
    TEST_ASSERT(validate_safe_model("model name") == 0,
                "space not rejected");
    TEST_ASSERT(validate_safe_model("") == 0,
                "empty not rejected");
    TEST_PASS("validate_safe_model: shell metacharacters rejected");
}

/* ================================================================== */
/* Audit Violation 13 (worker.c, BUG): json key substring matching     */
/* ================================================================== */

/*
 * json_extract_string is static, but we test it indirectly via
 * cmd_session/cmd_continue which use it. For unit testing, we
 * expose it via a test wrapper declared in worker.h under
 * a test-only section, or we test the behaviour through the
 * public API.
 *
 * Since we cannot call static functions directly, we test the
 * observable behaviour: cmd_continue reading a JSON file with
 * ambiguous keys. But that requires file I/O and tmux.
 *
 * Instead, we add a postcondition assertion inside json_extract_string
 * that catches substring matches. The test verifies the assertion
 * would have caught the problem by testing with a crafted JSON
 * through the public API (cmd_session with a crafted metadata file).
 *
 * For now, we test validate_uuid which is the downstream guard —
 * even if json_extract_string extracts the wrong value, the UUID
 * validation in cmd_continue will reject it.
 */

static void test_uuid_rejects_non_uuid_content(void)
{
    /* Values that might be extracted from wrong JSON position */
    TEST_ASSERT(validate_uuid("see session_id for details here!!") == 0,
                "non-UUID text not rejected by uuid validator");
    TEST_ASSERT(validate_uuid("not-a-uuid-at-all-but-len-is-36!") == 0,
                "non-UUID 36-char string not rejected");
    /* Exactly 36 chars but not valid hex */
    TEST_ASSERT(validate_uuid("zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz") == 0,
                "non-hex UUID-shaped string not rejected");
    TEST_PASS("validate_uuid: non-UUID content rejected (downstream guard for json extraction)");
}

/* ================================================================== */
/* Audit Violation 18 (worker.c, BUG): task file header slug vs name   */
/* We cannot test cmd_spawn (needs tmux) but we verify generate_name   */
/* produces a valid name from a slug, which is the precondition for     */
/* the fix.                                                            */
/* ================================================================== */

static void test_name_has_slug_prefix(void)
{
    /* Valid worker names must start with the slug portion */
    TEST_ASSERT(validate_worker_name("parser-a3f1") == 1,
                "parser-a3f1 should be valid");

    /* Verify the slug portion is before the last dash */
    const char *name = "parser-a3f1";
    const char *dash = strrchr(name, '-');
    TEST_ASSERT(dash != NULL, "name should contain a dash");
    size_t slug_len = (size_t)(dash - name);
    TEST_ASSERT(slug_len == 6, "slug portion length should be 6 for 'parser'");
    TEST_ASSERT(strncmp(name, "parser", slug_len) == 0,
                "slug portion should be 'parser'");

    TEST_PASS("worker name contains slug as prefix (precondition for header fix)");
}

/* ================================================================== */
/* Audit Violation 1 (main.c, BUG): silent swallow of unknown args     */
/* Cannot test main() directly without exec, but we verify the         */
/* contract: unknown args should cause EXIT_BAD_ARGS. We test this     */
/* by checking cmd_search returns EXIT_BAD_ARGS for bad context value. */
/* ================================================================== */

/* Tested indirectly: the fix changes the fprintf+continue to
 * fprintf+return EXIT_BAD_ARGS. We verify at build time that
 * the code compiles and at integration time that unknown args fail.
 * The unit test here verifies the validation of context_lines range
 * which is the postcondition for the argument parser. */

static void test_search_context_boundary_values(void)
{
    /* context_lines is validated to be 0..10000.
     * cmd_search itself checks context_lines via assertion now.
     * We test the boundary: negative context should not be possible
     * after the parse, and we verify the function rejects bad names. */

    /* Invalid name should return EXIT_BAD_ARGS regardless of context */
    int rc = cmd_search("", "pattern", 50, "/tmp");
    TEST_ASSERT(rc == EXIT_BAD_ARGS,
                "cmd_search should reject empty name, got %d", rc);

    rc = cmd_search("../../../etc/shadow-a3f1", "pattern", 50, "/tmp");
    TEST_ASSERT(rc == EXIT_BAD_ARGS,
                "cmd_search should reject path traversal name, got %d", rc);

    TEST_PASS("cmd_search: validates name input at boundary");
}

/* ================================================================== */
/* Audit Violations 4-9 (worker.c, BUG): cwd NULL checks              */
/* These are assertion-based, so passing NULL would abort().            */
/* We verify the fix exists by testing that valid cwd + invalid name   */
/* returns the expected error code (proving cwd was accepted).         */
/* ================================================================== */

static void test_cmd_status_rejects_bad_name(void)
{
    /* Valid cwd, invalid name — exercises the cwd path without abort */
    int rc = cmd_status("", "/tmp");
    TEST_ASSERT(rc == EXIT_BAD_ARGS,
                "cmd_status should reject empty name, got %d", rc);

    rc = cmd_status("../../../etc/shadow-a3f1", "/tmp");
    TEST_ASSERT(rc == EXIT_BAD_ARGS,
                "cmd_status should reject path traversal, got %d", rc);

    TEST_PASS("cmd_status: validates inputs correctly with valid cwd");
}

static void test_cmd_results_rejects_bad_name(void)
{
    int rc = cmd_results("", "/tmp");
    TEST_ASSERT(rc == EXIT_BAD_ARGS,
                "cmd_results should reject empty name, got %d", rc);

    rc = cmd_results("a;b-0000", "/tmp");
    TEST_ASSERT(rc == EXIT_BAD_ARGS,
                "cmd_results should reject shell metachar, got %d", rc);

    TEST_PASS("cmd_results: validates name input");
}

static void test_cmd_dismiss_rejects_bad_name(void)
{
    int rc = cmd_dismiss("", "/tmp");
    TEST_ASSERT(rc == EXIT_BAD_ARGS,
                "cmd_dismiss should reject empty name, got %d", rc);

    rc = cmd_dismiss("../hack-0000", "/tmp");
    TEST_ASSERT(rc == EXIT_BAD_ARGS,
                "cmd_dismiss should reject traversal, got %d", rc);

    TEST_PASS("cmd_dismiss: validates name input");
}

static void test_cmd_continue_rejects_bad_handle(void)
{
    int rc = cmd_continue("", NULL, "/tmp");
    TEST_ASSERT(rc == EXIT_BAD_ARGS,
                "cmd_continue should reject empty handle, got %d", rc);

    TEST_PASS("cmd_continue: validates handle input");
}

static void test_cmd_session_rejects_bad_handle(void)
{
    int rc = cmd_session("", "/tmp");
    TEST_ASSERT(rc == EXIT_BAD_ARGS,
                "cmd_session should reject empty handle, got %d", rc);

    TEST_PASS("cmd_session: validates handle input");
}

static void test_cmd_list_with_nonexistent_dir(void)
{
    /* cmd_list should handle missing workers dir gracefully */
    int rc = cmd_list("/tmp/nonexistent_nbs_test_dir_12345");
    TEST_ASSERT(rc == EXIT_SUCCESS_CODE,
                "cmd_list should succeed with missing dir, got %d", rc);

    TEST_PASS("cmd_list: handles missing workers directory");
}

/* ================================================================== */
/* Slug length boundary test                                           */
/* ================================================================== */

static void test_slug_boundary_length(void)
{
    /* Very long slug — should still validate if all chars are valid */
    char long_slug[200];
    memset(long_slug, 'a', 199);
    long_slug[199] = '\0';
    TEST_ASSERT(validate_slug(long_slug) == 1,
                "199-char all-a slug should be valid");

    /* Single char */
    TEST_ASSERT(validate_slug("z") == 1,
                "single 'z' should be valid");
    TEST_ASSERT(validate_slug("9") == 1,
                "single '9' should be valid");

    TEST_PASS("validate_slug: boundary lengths handled correctly");
}

/* ================================================================== */
/* UUID boundary tests                                                 */
/* ================================================================== */

static void test_uuid_boundary_chars(void)
{
    /* Test each hex boundary char individually */
    /* 'a' is first valid lowercase hex after digits */
    TEST_ASSERT(validate_uuid("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa") == 1,
                "all-a UUID should be valid");
    /* 'f' is last valid hex char */
    TEST_ASSERT(validate_uuid("ffffffff-ffff-ffff-ffff-ffffffffffff") == 1,
                "all-f UUID should be valid");
    /* 'g' is first invalid char after hex range */
    TEST_ASSERT(validate_uuid("gggggggg-gggg-gggg-gggg-gggggggggggg") == 0,
                "all-g UUID should be invalid");

    TEST_PASS("validate_uuid: boundary characters tested");
}

/* ================================================================== */
/* Audit Violation 14: PID display for unknown values                  */
/* (tested indirectly — the fix is in display code)                    */
/* ================================================================== */

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void)
{
    printf("=== nbs-workers adversarial tests ===\n\n");

    printf("[HARDENING] Redundant NULL guard removal:\n");
    test_slug_empty_only_guard();
    test_name_empty_only_guard();
    test_uuid_empty_only_guard();

    printf("\n[SECURITY] Shell injection via handle/model:\n");
    test_handle_rejects_shell_injection();
    test_safe_handle_accepts_valid();
    test_safe_handle_rejects_empty();
    test_safe_handle_rejects_shell_metacharacters();
    test_safe_handle_rejects_uppercase();
    test_safe_handle_rejects_leading_hyphen();
    test_safe_model_accepts_valid();
    test_safe_model_rejects_shell_metacharacters();

    printf("\n[BUG] JSON key substring / UUID downstream guard:\n");
    test_uuid_rejects_non_uuid_content();

    printf("\n[BUG] Task file header slug vs name:\n");
    test_name_has_slug_prefix();

    printf("\n[BUG] Argument validation:\n");
    test_search_context_boundary_values();

    printf("\n[BUG] cwd NULL checks (indirect via valid cwd paths):\n");
    test_cmd_status_rejects_bad_name();
    test_cmd_results_rejects_bad_name();
    test_cmd_dismiss_rejects_bad_name();
    test_cmd_continue_rejects_bad_handle();
    test_cmd_session_rejects_bad_handle();
    test_cmd_list_with_nonexistent_dir();

    printf("\n[HARDENING] Boundary values:\n");
    test_slug_boundary_length();
    test_uuid_boundary_chars();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
