/*
 * test_workers_unit.c — Unit tests for nbs-workers validation functions.
 *
 * Tests validate_slug, validate_worker_name, and validate_uuid.
 * These are path traversal defences — the security boundary between
 * user-supplied names and filesystem paths.
 *
 * NOTE: All three functions have ASSERT_MSG(ptr != NULL) preconditions
 * that abort() on NULL input. The header comments say "handles NULL
 * gracefully" but the implementation aborts. This is a documentation
 * bug — the ASSERT_MSG is the real contract. NULL tests are omitted
 * because they would abort the test binary.
 *
 * Build (from src/nbs-workers/):
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 \
 *       -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2 \
 *       -I. -I../nbs-common \
 *       -o ../../tests/test_workers_unit \
 *       ../../tests/test_workers_unit.c worker.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* validate_slug tests                                                 */
/* ================================================================== */

static void test_slug_valid_lowercase(void)
{
    TEST_ASSERT(validate_slug("parser") == 1,
                "valid lowercase slug rejected");
    TEST_ASSERT(validate_slug("a") == 1,
                "single char slug rejected");
    TEST_ASSERT(validate_slug("worker") == 1,
                "common slug rejected");
    TEST_PASS("validate_slug: valid lowercase strings accepted");
}

static void test_slug_valid_with_digits(void)
{
    TEST_ASSERT(validate_slug("worker1") == 1,
                "slug with trailing digit rejected");
    TEST_ASSERT(validate_slug("1worker") == 1,
                "slug with leading digit rejected");
    TEST_ASSERT(validate_slug("abc123") == 1,
                "mixed alpha-digit slug rejected");
    TEST_ASSERT(validate_slug("0") == 1,
                "single digit slug rejected");
    TEST_ASSERT(validate_slug("0123456789") == 1,
                "all-digit slug rejected");
    TEST_PASS("validate_slug: strings with digits accepted");
}

static void test_slug_rejects_empty(void)
{
    TEST_ASSERT(validate_slug("") == 0,
                "empty string not rejected");
    TEST_PASS("validate_slug: empty string rejected");
}

static void test_slug_rejects_uppercase(void)
{
    TEST_ASSERT(validate_slug("Parser") == 0,
                "uppercase not rejected");
    TEST_ASSERT(validate_slug("PARSER") == 0,
                "all-uppercase not rejected");
    TEST_ASSERT(validate_slug("parsEr") == 0,
                "mixed case not rejected");
    TEST_PASS("validate_slug: uppercase rejected");
}

static void test_slug_rejects_special_chars(void)
{
    TEST_ASSERT(validate_slug("parse-r") == 0,
                "hyphen not rejected");
    TEST_ASSERT(validate_slug("parse_r") == 0,
                "underscore not rejected");
    TEST_ASSERT(validate_slug("parse.r") == 0,
                "dot not rejected");
    TEST_ASSERT(validate_slug("parse r") == 0,
                "space not rejected");
    TEST_ASSERT(validate_slug("parse/r") == 0,
                "slash not rejected");
    TEST_PASS("validate_slug: special characters rejected");
}

static void test_slug_rejects_path_traversal(void)
{
    TEST_ASSERT(validate_slug("..") == 0,
                "double dot not rejected");
    TEST_ASSERT(validate_slug("../etc/passwd") == 0,
                "path traversal not rejected");
    TEST_ASSERT(validate_slug("/absolute") == 0,
                "absolute path not rejected");
    TEST_ASSERT(validate_slug("./relative") == 0,
                "relative path not rejected");
    TEST_PASS("validate_slug: path traversal patterns rejected");
}

static void test_slug_rejects_shell_metacharacters(void)
{
    TEST_ASSERT(validate_slug("a;b") == 0,
                "semicolon not rejected");
    TEST_ASSERT(validate_slug("a`b") == 0,
                "backtick not rejected");
    TEST_ASSERT(validate_slug("a$(b)") == 0,
                "command substitution not rejected");
    TEST_ASSERT(validate_slug("a|b") == 0,
                "pipe not rejected");
    TEST_ASSERT(validate_slug("a&b") == 0,
                "ampersand not rejected");
    TEST_ASSERT(validate_slug("a\nb") == 0,
                "newline not rejected");
    TEST_PASS("validate_slug: shell metacharacters rejected");
}

/* ================================================================== */
/* validate_worker_name tests                                          */
/* ================================================================== */

static void test_name_valid(void)
{
    TEST_ASSERT(validate_worker_name("parser-a3f1") == 1,
                "valid worker name rejected");
    TEST_ASSERT(validate_worker_name("worker-0000") == 1,
                "all-zero hex rejected");
    TEST_ASSERT(validate_worker_name("worker-ffff") == 1,
                "all-f hex rejected");
    TEST_ASSERT(validate_worker_name("a-0000") == 1,
                "single-char slug rejected");
    TEST_ASSERT(validate_worker_name("abc123-dead") == 1,
                "slug with digits rejected");
    TEST_PASS("validate_worker_name: valid names accepted");
}

static void test_name_rejects_empty(void)
{
    TEST_ASSERT(validate_worker_name("") == 0,
                "empty string not rejected");
    TEST_PASS("validate_worker_name: empty string rejected");
}

static void test_name_rejects_no_dash(void)
{
    TEST_ASSERT(validate_worker_name("parsera3f1") == 0,
                "missing dash not rejected");
    TEST_PASS("validate_worker_name: missing dash rejected");
}

static void test_name_rejects_dash_at_start(void)
{
    TEST_ASSERT(validate_worker_name("-a3f1") == 0,
                "dash at start (empty slug) not rejected");
    TEST_PASS("validate_worker_name: dash at start rejected");
}

static void test_name_rejects_wrong_hex_length(void)
{
    TEST_ASSERT(validate_worker_name("parser-a3f") == 0,
                "3-char hex not rejected");
    TEST_ASSERT(validate_worker_name("parser-a3f12") == 0,
                "5-char hex not rejected");
    TEST_ASSERT(validate_worker_name("parser-") == 0,
                "empty hex not rejected");
    TEST_PASS("validate_worker_name: wrong hex suffix length rejected");
}

static void test_name_rejects_uppercase_hex(void)
{
    TEST_ASSERT(validate_worker_name("parser-A3F1") == 0,
                "uppercase hex not rejected");
    TEST_ASSERT(validate_worker_name("parser-A3f1") == 0,
                "mixed-case hex not rejected");
    TEST_PASS("validate_worker_name: uppercase hex rejected");
}

static void test_name_rejects_invalid_slug_portion(void)
{
    TEST_ASSERT(validate_worker_name("Par-a3f1") == 0,
                "uppercase slug not rejected");
    TEST_ASSERT(validate_worker_name("par_ser-a3f1") == 0,
                "underscore in slug not rejected");
    TEST_ASSERT(validate_worker_name("par.ser-a3f1") == 0,
                "dot in slug not rejected");
    TEST_ASSERT(validate_worker_name("par ser-a3f1") == 0,
                "space in slug not rejected");
    TEST_PASS("validate_worker_name: invalid slug portion rejected");
}

static void test_name_rejects_path_traversal(void)
{
    TEST_ASSERT(validate_worker_name("../etc/passwd-a3f1") == 0,
                "path traversal in slug not rejected");
    TEST_ASSERT(validate_worker_name("/etc/shadow-a3f1") == 0,
                "absolute path in slug not rejected");
    TEST_ASSERT(validate_worker_name("..-..-a3f1") == 0,
                "double-dot slug not rejected");
    TEST_PASS("validate_worker_name: path traversal rejected");
}

static void test_name_multiple_dashes(void)
{
    /* strrchr finds the LAST dash, so "parser-worker-a3f1" should work:
     * slug="parser-worker" (invalid because of inner dash? No — slug
     * portion allows [a-z0-9] only, so the inner dash fails validation) */
    TEST_ASSERT(validate_worker_name("parser-worker-a3f1") == 0,
                "inner dash in slug portion not rejected");

    /* But "parserworker-a3f1" should work */
    TEST_ASSERT(validate_worker_name("parserworker-a3f1") == 1,
                "valid long slug rejected");
    TEST_PASS("validate_worker_name: multiple dashes handled correctly");
}

static void test_name_rejects_non_hex_suffix(void)
{
    TEST_ASSERT(validate_worker_name("parser-gggg") == 0,
                "non-hex chars (g) not rejected");
    TEST_ASSERT(validate_worker_name("parser-zzzz") == 0,
                "non-hex chars (z) not rejected");
    TEST_ASSERT(validate_worker_name("parser-a3g1") == 0,
                "non-hex char (g) in suffix not rejected");
    TEST_PASS("validate_worker_name: non-hex suffix rejected");
}

/* ================================================================== */
/* validate_uuid tests                                                 */
/* ================================================================== */

static void test_uuid_valid(void)
{
    TEST_ASSERT(validate_uuid("550e8400-e29b-41d4-a716-446655440000") == 1,
                "valid UUID rejected");
    TEST_ASSERT(validate_uuid("00000000-0000-0000-0000-000000000000") == 1,
                "nil UUID rejected");
    TEST_ASSERT(validate_uuid("ffffffff-ffff-ffff-ffff-ffffffffffff") == 1,
                "max UUID rejected");
    TEST_PASS("validate_uuid: valid UUIDs accepted");
}

static void test_uuid_rejects_empty(void)
{
    TEST_ASSERT(validate_uuid("") == 0,
                "empty string not rejected");
    TEST_PASS("validate_uuid: empty string rejected");
}

static void test_uuid_rejects_wrong_length(void)
{
    TEST_ASSERT(validate_uuid("550e8400-e29b-41d4-a716-44665544000") == 0,
                "35-char UUID not rejected");
    TEST_ASSERT(validate_uuid("550e8400-e29b-41d4-a716-4466554400000") == 0,
                "37-char UUID not rejected");
    TEST_ASSERT(validate_uuid("short") == 0,
                "very short string not rejected");
    TEST_PASS("validate_uuid: wrong length rejected");
}

static void test_uuid_rejects_uppercase(void)
{
    TEST_ASSERT(validate_uuid("550E8400-E29B-41D4-A716-446655440000") == 0,
                "uppercase UUID not rejected");
    TEST_ASSERT(validate_uuid("550e8400-e29b-41d4-a716-44665544000F") == 0,
                "trailing uppercase F not rejected");
    TEST_PASS("validate_uuid: uppercase hex rejected");
}

static void test_uuid_rejects_missing_dashes(void)
{
    TEST_ASSERT(validate_uuid("550e8400e29b41d4a716446655440000xxxx") == 0,
                "no dashes not rejected");
    /* Dash in wrong position */
    TEST_ASSERT(validate_uuid("550e840-0e29b-41d4-a716-446655440000") == 0,
                "dash in wrong position not rejected");
    TEST_PASS("validate_uuid: missing or misplaced dashes rejected");
}

static void test_uuid_rejects_non_hex(void)
{
    TEST_ASSERT(validate_uuid("550e8400-e29b-41d4-a716-44665544000g") == 0,
                "non-hex char (g) not rejected");
    TEST_ASSERT(validate_uuid("550e8400-e29b-41d4-a716-44665544000z") == 0,
                "non-hex char (z) not rejected");
    TEST_ASSERT(validate_uuid("550e8400-e29b-41d4-a716-44665544000!") == 0,
                "special char not rejected");
    TEST_PASS("validate_uuid: non-hex characters rejected");
}

static void test_uuid_rejects_path_traversal(void)
{
    /* 36 chars long but with path traversal chars */
    TEST_ASSERT(validate_uuid("../../../../../etc/passwd/xxxxxxxxxx") == 0,
                "path traversal not rejected");
    TEST_PASS("validate_uuid: path traversal patterns rejected");
}

static void test_uuid_rejects_spaces(void)
{
    TEST_ASSERT(validate_uuid("550e8400-e29b-41d4-a716-44665544000 ") == 0,
                "trailing space not rejected");
    TEST_ASSERT(validate_uuid(" 550e8400-e29b-41d4-a716-44665544000") == 0,
                "leading space not rejected");
    TEST_PASS("validate_uuid: strings with spaces rejected");
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void)
{
    printf("=== nbs-workers validation unit tests ===\n\n");

    /* validate_slug */
    test_slug_valid_lowercase();
    test_slug_valid_with_digits();
    test_slug_rejects_empty();
    test_slug_rejects_uppercase();
    test_slug_rejects_special_chars();
    test_slug_rejects_path_traversal();
    test_slug_rejects_shell_metacharacters();

    /* validate_worker_name */
    test_name_valid();
    test_name_rejects_empty();
    test_name_rejects_no_dash();
    test_name_rejects_dash_at_start();
    test_name_rejects_wrong_hex_length();
    test_name_rejects_uppercase_hex();
    test_name_rejects_invalid_slug_portion();
    test_name_rejects_path_traversal();
    test_name_multiple_dashes();
    test_name_rejects_non_hex_suffix();

    /* validate_uuid */
    test_uuid_valid();
    test_uuid_rejects_empty();
    test_uuid_rejects_wrong_length();
    test_uuid_rejects_uppercase();
    test_uuid_rejects_missing_dashes();
    test_uuid_rejects_non_hex();
    test_uuid_rejects_path_traversal();
    test_uuid_rejects_spaces();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
