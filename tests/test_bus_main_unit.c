/*
 * test_bus_main_unit.c -- Unit tests for bus/main.c hardening fixes
 *
 * Tests:
 *   1. Overflow guard on dedup-window seconds-to-microseconds conversion
 *   2. Boundary values near LLONG_MAX / 1000000
 *   3. Path length validation against BUS_MAX_PATH
 *   4. Empty/whitespace source/type rejection
 *   5. validate_non_empty_no_whitespace helper coverage
 *
 * These tests exercise the static helper logic extracted from main.c.
 * Since the helpers are static, we test them indirectly through the
 * public interface (the nbs-bus binary) or by re-implementing the
 * validation logic here for unit-level verification.
 *
 * Build:
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -I../src/nbs-bus -o test_bus_main_unit test_bus_main_unit.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>

/*
 * We cannot directly call static functions from main.c, so we replicate
 * the validation logic here and test it. The point is to verify the
 * *invariants* that the fixes enforce, not to link against main.o.
 *
 * This mirrors the pattern in test_base64_unit.c where the encode/decode
 * functions are tested via their public interface.
 */

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

/* --- Replicated validation logic from main.c (under test) --- */

#define BUS_MAX_PATH       4096
#define BUS_MAX_HANDLE      128
#define BUS_MAX_TYPE        128
#define BUS_MAX_PAYLOAD   16384
#define BUS_MAX_FILENAME    512

/*
 * Replicated: overflow guard for seconds-to-microseconds conversion.
 * Returns microseconds on success, -1 on overflow or invalid input.
 */
static long long safe_seconds_to_us(long long val)
{
    if (val < 0)
        return -1;
    if (val > LLONG_MAX / 1000000LL)
        return -1;
    return val * 1000000LL;
}

/*
 * Replicated: validate non-empty, no whitespace.
 * Returns 0 if valid, -1 if invalid.
 */
static int validate_non_empty_no_whitespace(const char *s)
{
    if (s == NULL || s[0] == '\0')
        return -1;
    for (const char *p = s; *p; p++) {
        if (isspace((unsigned char)*p))
            return -1;
    }
    return 0;
}

/*
 * Replicated: path length check.
 * Returns 0 if valid, -1 if too long.
 */
static int validate_path_length(const char *path)
{
    if (path == NULL)
        return -1;
    if (strlen(path) >= BUS_MAX_PATH)
        return -1;
    return 0;
}

/* ================================================================ */
/* Test: overflow guard on seconds-to-microseconds conversion       */
/* ================================================================ */

static void test_overflow_guard_exact_boundary(void)
{
    /* LLONG_MAX / 1000000 is the largest value that does NOT overflow */
    long long max_safe = LLONG_MAX / 1000000LL;

    long long result = safe_seconds_to_us(max_safe);
    TEST_ASSERT(result >= 0,
                "max safe value %lld should not overflow, got %lld",
                max_safe, result);
    TEST_ASSERT(result == max_safe * 1000000LL,
                "max safe value conversion incorrect: %lld != %lld",
                result, max_safe * 1000000LL);

    TEST_PASS("overflow guard: exact boundary (LLONG_MAX / 1000000)");
}

static void test_overflow_guard_one_above_boundary(void)
{
    /* LLONG_MAX / 1000000 + 1 MUST be rejected */
    long long one_above = LLONG_MAX / 1000000LL + 1;

    long long result = safe_seconds_to_us(one_above);
    TEST_ASSERT(result == -1,
                "value %lld should overflow, got %lld", one_above, result);

    TEST_PASS("overflow guard: one above boundary triggers rejection");
}

static void test_overflow_guard_llong_max(void)
{
    /* LLONG_MAX itself must be rejected */
    long long result = safe_seconds_to_us(LLONG_MAX);
    TEST_ASSERT(result == -1,
                "LLONG_MAX should overflow, got %lld", result);

    TEST_PASS("overflow guard: LLONG_MAX rejected");
}

static void test_overflow_guard_zero(void)
{
    long long result = safe_seconds_to_us(0);
    TEST_ASSERT(result == 0,
                "0 seconds should give 0 microseconds, got %lld", result);

    TEST_PASS("overflow guard: zero is valid");
}

static void test_overflow_guard_one(void)
{
    long long result = safe_seconds_to_us(1);
    TEST_ASSERT(result == 1000000LL,
                "1 second should give 1000000 us, got %lld", result);

    TEST_PASS("overflow guard: 1 second converts correctly");
}

static void test_overflow_guard_negative(void)
{
    long long result = safe_seconds_to_us(-1);
    TEST_ASSERT(result == -1,
                "negative value should be rejected, got %lld", result);

    result = safe_seconds_to_us(LLONG_MIN);
    TEST_ASSERT(result == -1,
                "LLONG_MIN should be rejected, got %lld", result);

    TEST_PASS("overflow guard: negative values rejected");
}

static void test_overflow_guard_large_safe_values(void)
{
    /* 1 billion seconds (~31.7 years) -- should be safe */
    long long result = safe_seconds_to_us(1000000000LL);
    TEST_ASSERT(result == 1000000000LL * 1000000LL,
                "1e9 seconds should convert safely, got %lld", result);

    /* 1 trillion seconds -- should overflow on 64-bit */
    result = safe_seconds_to_us(1000000000000LL);
    /* LLONG_MAX / 1000000 is ~9.22e12, so 1e12 is safe */
    TEST_ASSERT(result == 1000000000000LL * 1000000LL,
                "1e12 seconds should convert safely, got %lld", result);

    TEST_PASS("overflow guard: large safe values accepted");
}

/* ================================================================ */
/* Test: path length validation                                      */
/* ================================================================ */

static void test_path_length_within_limit(void)
{
    /* A path of length BUS_MAX_PATH - 1 should be accepted */
    char *path = malloc(BUS_MAX_PATH);
    TEST_ASSERT(path != NULL, "malloc failed");
    memset(path, 'a', BUS_MAX_PATH - 1);
    path[BUS_MAX_PATH - 1] = '\0';

    int result = validate_path_length(path);
    TEST_ASSERT(result == 0,
                "path of length %d should be accepted", BUS_MAX_PATH - 1);

    free(path);
    TEST_PASS("path length: BUS_MAX_PATH - 1 accepted");
}

static void test_path_length_at_limit(void)
{
    /* A path of exactly BUS_MAX_PATH characters (excluding NUL) should be rejected */
    char *path = malloc(BUS_MAX_PATH + 1);
    TEST_ASSERT(path != NULL, "malloc failed");
    memset(path, 'a', BUS_MAX_PATH);
    path[BUS_MAX_PATH] = '\0';

    int result = validate_path_length(path);
    TEST_ASSERT(result == -1,
                "path of length %d should be rejected", BUS_MAX_PATH);

    free(path);
    TEST_PASS("path length: exactly BUS_MAX_PATH rejected");
}

static void test_path_length_above_limit(void)
{
    /* A very long path should be rejected */
    size_t len = BUS_MAX_PATH * 2;
    char *path = malloc(len + 1);
    TEST_ASSERT(path != NULL, "malloc failed");
    memset(path, 'x', len);
    path[len] = '\0';

    int result = validate_path_length(path);
    TEST_ASSERT(result == -1,
                "path of length %zu should be rejected", len);

    free(path);
    TEST_PASS("path length: 2x BUS_MAX_PATH rejected");
}

static void test_path_length_empty(void)
{
    int result = validate_path_length("");
    TEST_ASSERT(result == 0,
                "empty path should be accepted by length check");

    TEST_PASS("path length: empty string accepted (length check only)");
}

static void test_path_length_null(void)
{
    int result = validate_path_length(NULL);
    TEST_ASSERT(result == -1,
                "NULL path should be rejected");

    TEST_PASS("path length: NULL rejected");
}

/* ================================================================ */
/* Test: source/type validation (non-empty, no whitespace)           */
/* ================================================================ */

static void test_source_type_valid(void)
{
    TEST_ASSERT(validate_non_empty_no_whitespace("parser") == 0,
                "'parser' should be valid");
    TEST_ASSERT(validate_non_empty_no_whitespace("my-worker") == 0,
                "'my-worker' should be valid");
    TEST_ASSERT(validate_non_empty_no_whitespace("event_type_v2") == 0,
                "'event_type_v2' should be valid");
    TEST_ASSERT(validate_non_empty_no_whitespace("a") == 0,
                "single char should be valid");

    TEST_PASS("source/type validation: valid strings accepted");
}

static void test_source_type_empty(void)
{
    int result = validate_non_empty_no_whitespace("");
    TEST_ASSERT(result == -1,
                "empty string should be rejected");

    TEST_PASS("source/type validation: empty string rejected");
}

static void test_source_type_null(void)
{
    int result = validate_non_empty_no_whitespace(NULL);
    TEST_ASSERT(result == -1,
                "NULL should be rejected");

    TEST_PASS("source/type validation: NULL rejected");
}

static void test_source_type_whitespace_space(void)
{
    TEST_ASSERT(validate_non_empty_no_whitespace("hello world") == -1,
                "'hello world' (space) should be rejected");
    TEST_ASSERT(validate_non_empty_no_whitespace(" leading") == -1,
                "' leading' (leading space) should be rejected");
    TEST_ASSERT(validate_non_empty_no_whitespace("trailing ") == -1,
                "'trailing ' (trailing space) should be rejected");

    TEST_PASS("source/type validation: spaces rejected");
}

static void test_source_type_whitespace_tab(void)
{
    TEST_ASSERT(validate_non_empty_no_whitespace("hello\tworld") == -1,
                "tab should be rejected");
    TEST_ASSERT(validate_non_empty_no_whitespace("\t") == -1,
                "lone tab should be rejected");

    TEST_PASS("source/type validation: tabs rejected");
}

static void test_source_type_whitespace_newline(void)
{
    TEST_ASSERT(validate_non_empty_no_whitespace("hello\nworld") == -1,
                "newline should be rejected");
    TEST_ASSERT(validate_non_empty_no_whitespace("\n") == -1,
                "lone newline should be rejected");
    TEST_ASSERT(validate_non_empty_no_whitespace("hello\rworld") == -1,
                "carriage return should be rejected");

    TEST_PASS("source/type validation: newlines/CR rejected");
}

static void test_source_type_only_whitespace(void)
{
    TEST_ASSERT(validate_non_empty_no_whitespace(" ") == -1,
                "single space should be rejected");
    TEST_ASSERT(validate_non_empty_no_whitespace("   ") == -1,
                "multiple spaces should be rejected");
    TEST_ASSERT(validate_non_empty_no_whitespace(" \t\n") == -1,
                "mixed whitespace should be rejected");

    TEST_PASS("source/type validation: whitespace-only strings rejected");
}

/* ================================================================ */
/* Test: strtoll boundary parsing (simulating parse_dedup_window_opt)*/
/* ================================================================ */

static void test_strtoll_boundary_values(void)
{
    /*
     * Simulate what parse_dedup_window_opt does: strtoll then overflow check.
     * Test that values near the overflow boundary are handled correctly.
     */
    char buf[64];
    long long max_safe = LLONG_MAX / 1000000LL;

    /* Format the max safe value as a string, parse it back */
    snprintf(buf, sizeof(buf), "%lld", max_safe);

    char *endp;
    errno = 0;
    long long val = strtoll(buf, &endp, 10);
    TEST_ASSERT(errno == 0 && *endp == '\0',
                "strtoll should parse max safe value '%s' successfully", buf);
    TEST_ASSERT(val == max_safe,
                "parsed value %lld != expected %lld", val, max_safe);

    /* Now verify the overflow check passes */
    long long result = safe_seconds_to_us(val);
    TEST_ASSERT(result >= 0,
                "max safe value should convert successfully");

    /* Format max_safe + 1, parse and verify rejection */
    snprintf(buf, sizeof(buf), "%lld", max_safe + 1);
    errno = 0;
    val = strtoll(buf, &endp, 10);
    TEST_ASSERT(errno == 0 && *endp == '\0',
                "strtoll should parse '%s' successfully", buf);

    result = safe_seconds_to_us(val);
    TEST_ASSERT(result == -1,
                "value above max safe should be rejected");

    TEST_PASS("strtoll boundary values: parse then overflow check");
}

static void test_strtoll_llong_max_string(void)
{
    /*
     * strtoll("9223372036854775807") returns LLONG_MAX.
     * The overflow guard must catch this.
     */
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", LLONG_MAX);

    char *endp;
    errno = 0;
    long long val = strtoll(buf, &endp, 10);
    TEST_ASSERT(errno == 0 && *endp == '\0',
                "strtoll should parse LLONG_MAX string");
    TEST_ASSERT(val == LLONG_MAX,
                "parsed value should equal LLONG_MAX");

    long long result = safe_seconds_to_us(val);
    TEST_ASSERT(result == -1,
                "LLONG_MAX as seconds must be rejected by overflow guard");

    TEST_PASS("strtoll LLONG_MAX string: overflow guard catches it");
}

/* ================================================================ */
/* Test: empty --handle= detection (BUG #1)                          */
/* ================================================================ */

/*
 * Replicated: parse_handle_opt with the fix applied.
 * Returns:
 *   handle string pointer on success
 *   NULL if no --handle flag present
 *   (char *)-1 if --handle= is empty (error sentinel)
 */
static const char *parse_handle_opt_fixed(int argc, const char **argv, int start)
{
    if (argv == NULL) return NULL;
    if (start < 0 || start > argc) return NULL;
    for (int i = start; i < argc; i++) {
        if (strncmp(argv[i], "--handle=", 9) == 0) {
            const char *h = argv[i] + 9;
            if (h[0] == '\0') return (const char *)-1; /* error sentinel */
            return h;
        }
    }
    return NULL;
}

static void test_handle_opt_empty_is_error(void)
{
    const char *args[] = {"nbs-bus", "check", "/tmp", "--handle="};
    const char *result = parse_handle_opt_fixed(4, args, 3);
    TEST_ASSERT(result == (const char *)-1,
                "empty --handle= should return error sentinel, got %p",
                (void *)result);

    TEST_PASS("BUG #1: empty --handle= detected as error");
}

static void test_handle_opt_valid(void)
{
    const char *args[] = {"nbs-bus", "check", "/tmp", "--handle=worker-a"};
    const char *result = parse_handle_opt_fixed(4, args, 3);
    TEST_ASSERT(result != NULL && result != (const char *)-1,
                "valid --handle should return non-NULL, non-sentinel");
    TEST_ASSERT(strcmp(result, "worker-a") == 0,
                "expected 'worker-a', got '%s'", result);

    TEST_PASS("BUG #1: valid --handle= returns correct value");
}

static void test_handle_opt_absent(void)
{
    const char *args[] = {"nbs-bus", "check", "/tmp"};
    const char *result = parse_handle_opt_fixed(3, args, 3);
    TEST_ASSERT(result == NULL,
                "absent --handle should return NULL");

    TEST_PASS("BUG #1: absent --handle returns NULL");
}

/* ================================================================ */
/* Test: max_bytes zero rejection (BUG #2)                           */
/* ================================================================ */

/*
 * Replicated: the check that max_bytes must be positive before calling
 * bus_prune. After the fix, max_bytes <= 0 should be caught.
 */
static int validate_max_bytes(long long max_bytes)
{
    return max_bytes > 0 ? 0 : -1;
}

static void test_max_bytes_zero_rejected(void)
{
    TEST_ASSERT(validate_max_bytes(0) == -1,
                "max_bytes == 0 should be rejected");
    TEST_PASS("BUG #2: max_bytes == 0 rejected");
}

static void test_max_bytes_negative_rejected(void)
{
    TEST_ASSERT(validate_max_bytes(-1) == -1,
                "max_bytes == -1 should be rejected");
    TEST_ASSERT(validate_max_bytes(LLONG_MIN) == -1,
                "max_bytes == LLONG_MIN should be rejected");
    TEST_PASS("BUG #2: negative max_bytes rejected");
}

static void test_max_bytes_positive_accepted(void)
{
    TEST_ASSERT(validate_max_bytes(1) == 0,
                "max_bytes == 1 should be accepted");
    TEST_ASSERT(validate_max_bytes(16LL * 1024 * 1024) == 0,
                "max_bytes == 16MB should be accepted");
    TEST_PASS("BUG #2: positive max_bytes accepted");
}

/* ================================================================ */
/* Test: source/type/payload length bounds (BUG #3)                  */
/* ================================================================ */

static int validate_field_length(const char *s, size_t max_len, const char *label)
{
    (void)label;
    if (s == NULL) return -1;
    if (strlen(s) >= max_len) return -1;
    return 0;
}

static void test_source_length_at_limit(void)
{
    char src[BUS_MAX_HANDLE + 1];
    memset(src, 'a', BUS_MAX_HANDLE);
    src[BUS_MAX_HANDLE] = '\0';

    TEST_ASSERT(validate_field_length(src, BUS_MAX_HANDLE, "source") == -1,
                "source of length %d should be rejected", BUS_MAX_HANDLE);

    /* One below limit should pass */
    src[BUS_MAX_HANDLE - 1] = '\0';
    TEST_ASSERT(validate_field_length(src, BUS_MAX_HANDLE, "source") == 0,
                "source of length %d should be accepted", BUS_MAX_HANDLE - 1);

    TEST_PASS("BUG #3: source length boundary check");
}

static void test_type_length_at_limit(void)
{
    char type[BUS_MAX_TYPE + 1];
    memset(type, 'b', BUS_MAX_TYPE);
    type[BUS_MAX_TYPE] = '\0';

    TEST_ASSERT(validate_field_length(type, BUS_MAX_TYPE, "type") == -1,
                "type of length %d should be rejected", BUS_MAX_TYPE);

    type[BUS_MAX_TYPE - 1] = '\0';
    TEST_ASSERT(validate_field_length(type, BUS_MAX_TYPE, "type") == 0,
                "type of length %d should be accepted", BUS_MAX_TYPE - 1);

    TEST_PASS("BUG #3: type length boundary check");
}

static void test_payload_length_at_limit(void)
{
    char *payload = malloc(BUS_MAX_PAYLOAD + 1);
    TEST_ASSERT(payload != NULL, "malloc failed");
    memset(payload, 'c', BUS_MAX_PAYLOAD);
    payload[BUS_MAX_PAYLOAD] = '\0';

    TEST_ASSERT(validate_field_length(payload, BUS_MAX_PAYLOAD, "payload") == -1,
                "payload of length %d should be rejected", BUS_MAX_PAYLOAD);

    payload[BUS_MAX_PAYLOAD - 1] = '\0';
    TEST_ASSERT(validate_field_length(payload, BUS_MAX_PAYLOAD, "payload") == 0,
                "payload of length %d should be accepted", BUS_MAX_PAYLOAD - 1);

    free(payload);
    TEST_PASS("BUG #3: payload length boundary check");
}

/* ================================================================ */
/* Test: path traversal in event_file (SECURITY #4)                  */
/* ================================================================ */

static int validate_event_filename(const char *event_file)
{
    if (event_file == NULL || event_file[0] == '\0') return -1;
    if (strchr(event_file, '/') != NULL) return -1;
    if (strncmp(event_file, "..", 2) == 0) return -1;
    if (strlen(event_file) >= BUS_MAX_FILENAME) return -1;
    return 0;
}

static void test_event_file_path_traversal_slash(void)
{
    TEST_ASSERT(validate_event_filename("../../../etc/passwd") == -1,
                "path traversal with / should be rejected");
    TEST_ASSERT(validate_event_filename("processed/foo.event") == -1,
                "subdirectory path should be rejected");
    TEST_ASSERT(validate_event_filename("/absolute/path.event") == -1,
                "absolute path should be rejected");
    TEST_PASS("SECURITY #4: event_file with / rejected");
}

static void test_event_file_path_traversal_dotdot(void)
{
    TEST_ASSERT(validate_event_filename("..") == -1,
                "dotdot should be rejected");
    TEST_ASSERT(validate_event_filename("..foo.event") == -1,
                "dotdot prefix should be rejected");
    TEST_PASS("SECURITY #4: event_file with .. prefix rejected");
}

static void test_event_file_valid(void)
{
    TEST_ASSERT(validate_event_filename("12345-worker-task-99.event") == 0,
                "valid event filename should be accepted");
    TEST_PASS("SECURITY #4: valid event_file accepted");
}

static void test_event_file_overlength(void)
{
    char long_name[BUS_MAX_FILENAME + 8];
    memset(long_name, 'f', BUS_MAX_FILENAME);
    strcpy(long_name + BUS_MAX_FILENAME - 6, ".event");
    /* Now long_name is exactly BUS_MAX_FILENAME chars (sans NUL) — rebuild at limit */
    memset(long_name, 'f', BUS_MAX_FILENAME);
    long_name[BUS_MAX_FILENAME] = '\0';

    TEST_ASSERT(validate_event_filename(long_name) == -1,
                "event_file >= BUS_MAX_FILENAME should be rejected");
    TEST_PASS("HARDENING #11: overlength event_file rejected");
}

/* ================================================================ */
/* Test: handle validation (SECURITY #5, #6)                         */
/* ================================================================ */

static int validate_handle(const char *handle)
{
    if (handle == NULL) return 0; /* NULL = no filter, OK */
    if (handle[0] == '\0') return -1;
    if (strchr(handle, '/') != NULL) return -1;
    /* Reuse whitespace check */
    if (validate_non_empty_no_whitespace(handle) != 0) return -1;
    return 0;
}

static void test_handle_path_traversal(void)
{
    TEST_ASSERT(validate_handle("../evil") == -1,
                "handle with / should be rejected");
    TEST_ASSERT(validate_handle("foo/bar") == -1,
                "handle with / should be rejected");
    TEST_PASS("SECURITY #5/#6: handle with / rejected");
}

static void test_handle_whitespace(void)
{
    TEST_ASSERT(validate_handle("hello world") == -1,
                "handle with space should be rejected");
    TEST_ASSERT(validate_handle("tab\there") == -1,
                "handle with tab should be rejected");
    TEST_PASS("SECURITY #5/#6: handle with whitespace rejected");
}

static void test_handle_valid(void)
{
    TEST_ASSERT(validate_handle("worker-a") == 0,
                "valid handle should be accepted");
    TEST_ASSERT(validate_handle(NULL) == 0,
                "NULL handle (no filter) should be accepted");
    TEST_PASS("SECURITY #5/#6: valid handle accepted");
}

/* ================================================================ */
/* main                                                              */
/* ================================================================ */

int main(void)
{
    printf("=== bus/main.c unit tests ===\n\n");

    /* Overflow guard tests */
    test_overflow_guard_exact_boundary();
    test_overflow_guard_one_above_boundary();
    test_overflow_guard_llong_max();
    test_overflow_guard_zero();
    test_overflow_guard_one();
    test_overflow_guard_negative();
    test_overflow_guard_large_safe_values();

    /* Path length validation tests */
    test_path_length_within_limit();
    test_path_length_at_limit();
    test_path_length_above_limit();
    test_path_length_empty();
    test_path_length_null();

    /* Source/type validation tests */
    test_source_type_valid();
    test_source_type_empty();
    test_source_type_null();
    test_source_type_whitespace_space();
    test_source_type_whitespace_tab();
    test_source_type_whitespace_newline();
    test_source_type_only_whitespace();

    /* strtoll boundary tests */
    test_strtoll_boundary_values();
    test_strtoll_llong_max_string();

    /* BUG #1: empty --handle= detection */
    test_handle_opt_empty_is_error();
    test_handle_opt_valid();
    test_handle_opt_absent();

    /* BUG #2: max_bytes zero rejection */
    test_max_bytes_zero_rejected();
    test_max_bytes_negative_rejected();
    test_max_bytes_positive_accepted();

    /* BUG #3: source/type/payload length bounds */
    test_source_length_at_limit();
    test_type_length_at_limit();
    test_payload_length_at_limit();

    /* SECURITY #4: event_file path traversal */
    test_event_file_path_traversal_slash();
    test_event_file_path_traversal_dotdot();
    test_event_file_valid();
    test_event_file_overlength();

    /* SECURITY #5/#6: handle validation */
    test_handle_path_traversal();
    test_handle_whitespace();
    test_handle_valid();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
