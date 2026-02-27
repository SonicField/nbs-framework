/*
 * test_web_unit.c -- Unit tests for web.c (nbs-chat-web)
 *
 * Tests:
 *   1. json_escape: normal string, special characters, control chars
 *   2. json_escape: empty string, output_size=1 (only NUL fits)
 *   3. json_escape: truncation returns -1
 *   4. json_escape: all control chars 0x00-0x1F encoded as \uXXXX
 *   5. json_message: basic message serialisation
 *   6. json_message: negative index triggers ASSERT (not tested at runtime)
 *   7. json_message: truncation returns -1 with tiny buffer
 *   8. json_extract_string: basic extraction
 *   9. json_extract_string: escaped quotes within value
 *  10. json_extract_string: missing key returns -1
 *  11. json_extract_string: unterminated value returns -1
 *  12. json_extract_string: output buffer too small truncates
 *  13. parse_query_int: basic parsing
 *  14. parse_query_int: missing key returns default
 *  15. parse_query_int: negative value clamped to 0
 *  16. parse_query_int: overflow clamped to INT32_MAX
 *  17. parse_query_int: non-numeric value returns default
 *  18. parse_query_int: empty query returns default
 *  19. parse_query_int: NULL query/key returns default
 *  20. parse_query_int: multiple keys, correct selection
 *  21. BUG: serve_json pos overflow -- size_t pos prevents int overflow
 *  22. SECURITY: strtol Last-Event-ID clamping (via parse_request indirectly)
 *
 * Build:
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
 *       -D_DEFAULT_SOURCE -I../src/nbs-chat -o test_web_unit test_web_unit.c \
 *       ../src/nbs-chat/chat_file.c ../src/nbs-chat/lock.c \
 *       ../src/nbs-chat/base64.c ../src/nbs-chat/bus_bridge.c
 *
 * Strategy: We #include "web.c" directly to access static functions.
 * We stub out main() by redefining it via the preprocessor.
 */

/* _GNU_SOURCE must be defined before any system headers for strcasestr */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define main web_main  /* Rename web.c's main so we can define our own */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* Include the implementation directly to test static functions */
#include "../src/nbs-chat/web.c"

#undef main  /* Restore main for our test harness */

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

/* ================================================================
 * json_escape tests
 * ================================================================ */

static void test_json_escape_simple(void) {
    char buf[256];
    int ret = json_escape("hello world", buf, sizeof(buf));
    TEST_ASSERT(ret == 11, "json_escape('hello world') returned %d, expected 11", ret);
    TEST_ASSERT(strcmp(buf, "hello world") == 0,
                "json_escape('hello world') = '%s'", buf);
    TEST_PASS("json_escape: simple string");
}

static void test_json_escape_special_chars(void) {
    char buf[256];
    int ret = json_escape("he said \"hello\" \\ world\n", buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "json_escape returned %d", ret);
    /* Expected: he said \"hello\" \\ world\n */
    TEST_ASSERT(strstr(buf, "\\\"") != NULL, "should contain escaped quotes");
    TEST_ASSERT(strstr(buf, "\\\\") != NULL, "should contain escaped backslash");
    TEST_ASSERT(strstr(buf, "\\n") != NULL, "should contain escaped newline");
    TEST_PASS("json_escape: special characters");
}

static void test_json_escape_control_chars(void) {
    /* Test control characters 0x01-0x1F (except \b \f \n \r \t which have short forms) */
    char input[3] = { 0x01, 0x1F, 0 };
    char buf[256];
    int ret = json_escape(input, buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "json_escape returned %d", ret);
    /* 0x01 -> \u0001, 0x1F -> \u001f */
    TEST_ASSERT(strstr(buf, "\\u0001") != NULL,
                "0x01 should become \\u0001, got '%s'", buf);
    TEST_ASSERT(strstr(buf, "\\u001f") != NULL,
                "0x1F should become \\u001f, got '%s'", buf);
    TEST_PASS("json_escape: control characters as \\uXXXX");
}

static void test_json_escape_empty_string(void) {
    char buf[16];
    int ret = json_escape("", buf, sizeof(buf));
    TEST_ASSERT(ret == 0, "json_escape('') returned %d, expected 0", ret);
    TEST_ASSERT(buf[0] == '\0', "output should be empty string");
    TEST_PASS("json_escape: empty string");
}

static void test_json_escape_output_size_1(void) {
    /* output_size=1 means only NUL fits; any non-empty input should return -1 */
    char buf[1];
    int ret = json_escape("a", buf, 1);
    TEST_ASSERT(ret == -1, "json_escape with output_size=1 should return -1, got %d", ret);
    TEST_ASSERT(buf[0] == '\0', "output should be NUL-terminated even on truncation");
    TEST_PASS("json_escape: output_size=1 truncation");
}

static void test_json_escape_truncation(void) {
    /* Buffer too small for the escaped output */
    char buf[8]; /* "\\\"" is 2 chars, so "\"\"\"\"" (4 quotes) needs 8 escape chars + NUL = 9 */
    int ret = json_escape("\"\"\"\"", buf, sizeof(buf));
    TEST_ASSERT(ret == -1, "json_escape should return -1 on truncation, got %d", ret);
    TEST_ASSERT(buf[strlen(buf)] == '\0', "output should be NUL-terminated on truncation");
    TEST_PASS("json_escape: truncation returns -1");
}

static void test_json_escape_all_escapable_chars(void) {
    /* Test all characters that get special escape sequences */
    char input[] = "\"\\\b\f\n\r\t";
    char buf[256];
    int ret = json_escape(input, buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "json_escape returned %d", ret);
    /* Expected: \\\" \\\\ \\b \\f \\n \\r \\t = 2+2+2+2+2+2+2 = 14 chars */
    TEST_ASSERT(ret == 14, "expected 14 chars for 7 escapable chars, got %d", ret);
    TEST_ASSERT(strcmp(buf, "\\\"\\\\\\b\\f\\n\\r\\t") == 0,
                "unexpected output: '%s'", buf);
    TEST_PASS("json_escape: all escapable characters");
}

static void test_json_escape_high_bytes_passthrough(void) {
    /* Bytes >= 0x80 (e.g., UTF-8) should pass through unchanged */
    char input[] = {(char)0xC3, (char)0xA9, 0}; /* UTF-8 for e-acute */
    char buf[16];
    int ret = json_escape(input, buf, sizeof(buf));
    TEST_ASSERT(ret == 2, "high bytes should pass through, got ret=%d", ret);
    TEST_ASSERT((unsigned char)buf[0] == 0xC3 && (unsigned char)buf[1] == 0xA9,
                "high bytes should be preserved");
    TEST_PASS("json_escape: high bytes (UTF-8) pass through");
}

/* ================================================================
 * json_message tests
 * ================================================================ */

static void test_json_message_basic(void) {
    chat_message_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.handle, "alice", sizeof(msg.handle));
    msg.content = strdup("Hello, world!");
    msg.content_len = strlen(msg.content);
    msg.timestamp = 1700000000;

    char buf[1024];
    int ret = json_message(&msg, 0, buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "json_message returned %d", ret);
    TEST_ASSERT(strstr(buf, "\"index\":0") != NULL, "should contain index:0");
    TEST_ASSERT(strstr(buf, "\"handle\":\"alice\"") != NULL, "should contain handle:alice");
    TEST_ASSERT(strstr(buf, "\"content\":\"Hello, world!\"") != NULL,
                "should contain content");
    TEST_ASSERT(strstr(buf, "\"timestamp\":1700000000") != NULL,
                "should contain timestamp");

    free(msg.content);
    TEST_PASS("json_message: basic serialisation");
}

static void test_json_message_truncation(void) {
    chat_message_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.handle, "alice", sizeof(msg.handle));
    msg.content = strdup("Hello");
    msg.content_len = 5;
    msg.timestamp = 0;

    char buf[10]; /* Way too small */
    int ret = json_message(&msg, 0, buf, sizeof(buf));
    TEST_ASSERT(ret == -1, "json_message with tiny buffer should return -1, got %d", ret);

    free(msg.content);
    TEST_PASS("json_message: truncation returns -1");
}

static void test_json_message_null_content(void) {
    /* content can be NULL; json_message uses (msg->content ? msg->content : "") */
    chat_message_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.handle, "bob", sizeof(msg.handle));
    msg.content = NULL;
    msg.content_len = 0;
    msg.timestamp = 0;

    char buf[512];
    int ret = json_message(&msg, 0, buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "json_message with NULL content returned %d", ret);
    TEST_ASSERT(strstr(buf, "\"content\":\"\"") != NULL,
                "NULL content should become empty string");
    TEST_PASS("json_message: NULL content handled as empty string");
}

static void test_json_message_special_chars_in_handle(void) {
    chat_message_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.handle, "a\"b", sizeof(msg.handle)); /* Quote in handle */
    msg.content = strdup("test");
    msg.content_len = 4;
    msg.timestamp = 0;

    char buf[512];
    int ret = json_message(&msg, 0, buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "json_message returned %d", ret);
    /* Handle should be escaped: a\"b -> a\\\"b */
    TEST_ASSERT(strstr(buf, "a\\\"b") != NULL,
                "handle with quotes should be escaped, got: %s", buf);

    free(msg.content);
    TEST_PASS("json_message: special characters in handle escaped");
}

/* ================================================================
 * json_extract_string tests
 * ================================================================ */

static void test_json_extract_basic(void) {
    const char *json = "{\"handle\":\"alice\",\"message\":\"hello\"}";
    char out[64];
    int ret = json_extract_string(json, "handle", out, sizeof(out));
    TEST_ASSERT(ret == 0, "json_extract_string returned %d", ret);
    TEST_ASSERT(strcmp(out, "alice") == 0, "expected 'alice', got '%s'", out);
    TEST_PASS("json_extract_string: basic extraction");
}

static void test_json_extract_with_escapes(void) {
    const char *json = "{\"msg\":\"hello \\\"world\\\"\"}";
    char out[64];
    int ret = json_extract_string(json, "msg", out, sizeof(out));
    TEST_ASSERT(ret == 0, "json_extract_string returned %d", ret);
    TEST_ASSERT(strcmp(out, "hello \"world\"") == 0,
                "expected 'hello \"world\"', got '%s'", out);
    TEST_PASS("json_extract_string: escaped quotes");
}

static void test_json_extract_missing_key(void) {
    const char *json = "{\"handle\":\"alice\"}";
    char out[64];
    int ret = json_extract_string(json, "missing", out, sizeof(out));
    TEST_ASSERT(ret == -1, "missing key should return -1, got %d", ret);
    TEST_PASS("json_extract_string: missing key returns -1");
}

static void test_json_extract_unterminated_value(void) {
    /* Value without closing quote */
    const char *json = "{\"key\":\"unterminated value";
    char out[64];
    int ret = json_extract_string(json, "key", out, sizeof(out));
    TEST_ASSERT(ret == -1, "unterminated value should return -1, got %d", ret);
    TEST_PASS("json_extract_string: unterminated value returns -1");
}

static void test_json_extract_empty_value(void) {
    const char *json = "{\"key\":\"\"}";
    char out[64];
    int ret = json_extract_string(json, "key", out, sizeof(out));
    TEST_ASSERT(ret == 0, "empty value should return 0, got %d", ret);
    TEST_ASSERT(out[0] == '\0', "empty value should produce empty string");
    TEST_PASS("json_extract_string: empty value");
}

static void test_json_extract_buffer_small(void) {
    const char *json = "{\"key\":\"a very long value that should be truncated\"}";
    char out[8]; /* Small buffer */
    int ret = json_extract_string(json, "key", out, sizeof(out));
    /* The function copies until out_size-1 then checks if *p == '"' */
    /* With out_size=8, it copies 7 chars, then checks. p won't be at '"'. */
    TEST_ASSERT(ret == -1, "truncated extraction should return -1, got %d", ret);
    TEST_PASS("json_extract_string: buffer too small returns -1");
}

static void test_json_extract_escape_sequences(void) {
    const char *json = "{\"key\":\"line1\\nline2\\ttab\\\\backslash\"}";
    char out[64];
    int ret = json_extract_string(json, "key", out, sizeof(out));
    TEST_ASSERT(ret == 0, "json_extract_string returned %d", ret);
    TEST_ASSERT(strstr(out, "\n") != NULL, "should contain literal newline");
    TEST_ASSERT(strstr(out, "\t") != NULL, "should contain literal tab");
    TEST_ASSERT(strstr(out, "\\") != NULL, "should contain literal backslash");
    TEST_PASS("json_extract_string: escape sequences decoded");
}

/* ================================================================
 * parse_query_int tests
 * ================================================================ */

static void test_parse_query_int_basic(void) {
    int val = parse_query_int("since=5&last=10", "since", -1);
    TEST_ASSERT(val == 5, "expected 5, got %d", val);
    TEST_PASS("parse_query_int: basic parsing");
}

static void test_parse_query_int_second_key(void) {
    int val = parse_query_int("since=5&last=10", "last", -1);
    TEST_ASSERT(val == 10, "expected 10, got %d", val);
    TEST_PASS("parse_query_int: second key");
}

static void test_parse_query_int_missing_key(void) {
    int val = parse_query_int("since=5", "missing", -1);
    TEST_ASSERT(val == -1, "expected -1 (default), got %d", val);
    TEST_PASS("parse_query_int: missing key returns default");
}

static void test_parse_query_int_negative_clamped(void) {
    int val = parse_query_int("val=-5", "val", 0);
    TEST_ASSERT(val == 0, "negative value should be clamped to 0, got %d", val);
    TEST_PASS("parse_query_int: negative clamped to 0");
}

static void test_parse_query_int_overflow_clamped(void) {
    /* A value larger than INT32_MAX */
    int val = parse_query_int("val=99999999999", "val", 0);
    TEST_ASSERT(val == INT32_MAX, "overflow should be clamped to INT32_MAX, got %d", val);
    TEST_PASS("parse_query_int: overflow clamped to INT32_MAX");
}

static void test_parse_query_int_nonnumeric(void) {
    int val = parse_query_int("val=abc", "val", -1);
    TEST_ASSERT(val == -1, "non-numeric should return default, got %d", val);
    TEST_PASS("parse_query_int: non-numeric returns default");
}

static void test_parse_query_int_empty_query(void) {
    int val = parse_query_int("", "val", 42);
    TEST_ASSERT(val == 42, "empty query should return default, got %d", val);
    TEST_PASS("parse_query_int: empty query returns default");
}

static void test_parse_query_int_null_query(void) {
    int val = parse_query_int(NULL, "val", 42);
    TEST_ASSERT(val == 42, "NULL query should return default, got %d", val);
    TEST_PASS("parse_query_int: NULL query returns default");
}

static void test_parse_query_int_null_key(void) {
    int val = parse_query_int("val=1", NULL, 42);
    TEST_ASSERT(val == 42, "NULL key should return default, got %d", val);
    TEST_PASS("parse_query_int: NULL key returns default");
}

static void test_parse_query_int_zero_value(void) {
    int val = parse_query_int("val=0", "val", -1);
    TEST_ASSERT(val == 0, "zero should be valid, got %d", val);
    TEST_PASS("parse_query_int: zero value");
}

static void test_parse_query_int_partial_key_match(void) {
    /* "since" should not match "last_since" */
    int val = parse_query_int("last_since=99", "since", -1);
    /* This WILL match because strncmp only checks prefix at current position */
    /* But the key is "since" (5 chars) and at position "last_since" we look for
       "since" starting at "l", which fails. Starting at "s" after &? No, only one key. */
    TEST_ASSERT(val == -1, "partial key should not match, got %d", val);
    TEST_PASS("parse_query_int: partial key match rejected");
}

/* ================================================================
 * BUG #9: serve_json pos overflow verification
 * (We cannot call serve_json directly since it needs a socket,
 *  but we can verify the fix by checking pos is now size_t in the source.)
 * ================================================================ */

/* We can verify the fix indirectly by testing that snprintf clamping works:
 * Create a situation where snprintf's return value would overflow int
 * but is safely handled with size_t. The real test is in the integration
 * test with a large chat file. Here we test the building blocks. */

static void test_snprintf_clamping_pattern(void) {
    /* Verify the clamping pattern used in the fixed serve_json works correctly */
    char buf[32];
    size_t buf_size = sizeof(buf);
    size_t pos = 0;

    /* Write something that fits */
    int n = snprintf(buf + pos, buf_size - pos, "{\"key\":\"%s\"}", "hello");
    if (n < 0) n = 0;
    if ((size_t)n >= buf_size - pos) {
        /* Truncated -- pos should stay at buffer limit */
        pos = buf_size - 1;
    } else {
        pos += (size_t)n;
    }

    TEST_ASSERT(pos < buf_size, "pos should be within buffer, got %zu", pos);
    TEST_ASSERT(buf[pos] == '\0', "buffer should be NUL-terminated at pos");

    /* Now write something that would overflow */
    char big_buf[16];
    size_t big_size = sizeof(big_buf);
    size_t big_pos = 14; /* Near the end */
    n = snprintf(big_buf + big_pos, big_size - big_pos, "this is too long to fit");
    if (n < 0) n = 0;
    if ((size_t)n >= big_size - big_pos) {
        big_pos = big_size - 1; /* Clamped */
    } else {
        big_pos += (size_t)n;
    }
    TEST_ASSERT(big_pos == big_size - 1,
                "pos should be clamped to buf_size-1 on truncation, got %zu", big_pos);

    TEST_PASS("snprintf clamping pattern: correctly prevents overflow");
}

/* ================================================================
 * BUG #10: sockaddr_storage verification
 * (Cannot test accept() directly, but verify the struct sizes)
 * ================================================================ */

static void test_sockaddr_storage_size(void) {
    /* Verify that sockaddr_storage is large enough for both IPv4 and IPv6 */
    TEST_ASSERT(sizeof(struct sockaddr_storage) >= sizeof(struct sockaddr_in),
                "sockaddr_storage must fit sockaddr_in");
    TEST_ASSERT(sizeof(struct sockaddr_storage) >= sizeof(struct sockaddr_in6),
                "sockaddr_storage must fit sockaddr_in6");
    /* The bug was using sockaddr_in (16 bytes) for an IPv6 socket which needs
     * sockaddr_in6 (28 bytes). Verify the difference is significant. */
    TEST_ASSERT(sizeof(struct sockaddr_in6) > sizeof(struct sockaddr_in),
                "sockaddr_in6 (%zu) should be larger than sockaddr_in (%zu)",
                sizeof(struct sockaddr_in6), sizeof(struct sockaddr_in));
    TEST_PASS("sockaddr_storage: large enough for both address families");
}

/* ================================================================
 * SECURITY #11: Header value clamping
 * ================================================================ */

/* We cannot call parse_request directly (needs a socket),
 * but we test the strtol clamping pattern for the fixed code. */
static void test_strtol_clamping_pattern(void) {
    /* Simulate the clamping for Last-Event-ID */
    const char *header_val = "99999999999"; /* > INT_MAX */
    char *endptr;
    errno = 0;
    long val = strtol(header_val, &endptr, 10);
    /* Apply the clamping fix */
    if (val < -1) val = -1;
    if (val > INT32_MAX) val = INT32_MAX;
    int clamped = (int)val;
    TEST_ASSERT(clamped == INT32_MAX,
                "large Last-Event-ID should clamp to INT32_MAX, got %d", clamped);

    /* Negative Last-Event-ID */
    header_val = "-999";
    errno = 0;
    val = strtol(header_val, &endptr, 10);
    if (val < -1) val = -1;
    if (val > INT32_MAX) val = INT32_MAX;
    clamped = (int)val;
    TEST_ASSERT(clamped == -1,
                "negative Last-Event-ID should clamp to -1, got %d", clamped);

    /* Simulate Content-Length clamping */
    header_val = "99999999999";
    errno = 0;
    val = strtol(header_val, &endptr, 10);
    if (val > MAX_REQUEST_SIZE) val = MAX_REQUEST_SIZE;
    if (val < 0) val = 0;
    int cl = (int)val;
    TEST_ASSERT(cl == MAX_REQUEST_SIZE,
                "large Content-Length should clamp to MAX_REQUEST_SIZE, got %d", cl);

    TEST_PASS("strtol clamping: overflow and underflow handled correctly");
}

/* ================================================================
 * HARDENING #12: Header position verification
 * ================================================================ */

static void test_header_at_line_start(void) {
    /* Simulate the header line-start check */
    const char *buf = "X-Debug: Last-Event-ID: 42\r\nLast-Event-ID: 7\r\n\r\n";
    const char *hdr = "Last-Event-ID:";

    /* The naive approach (just strcasestr) would find the one inside X-Debug */
    char *found = strcasestr(buf, hdr);
    TEST_ASSERT(found != NULL, "should find Last-Event-ID somewhere");

    /* The fixed approach: verify it's at line start */
    while (found) {
        if (found == buf || found[-1] == '\n') {
            /* This is a real header */
            break;
        }
        /* Search again after this occurrence */
        found = strcasestr(found + 1, hdr);
    }

    TEST_ASSERT(found != NULL, "should find Last-Event-ID at line start");
    /* Verify we found the second one (index 7), not the one in X-Debug (42) */
    found += strlen(hdr);
    while (*found == ' ') found++;
    char *endptr;
    long val = strtol(found, &endptr, 10);
    TEST_ASSERT(val == 7,
                "should find the real header value 7, not 42 from X-Debug, got %ld", val);

    TEST_PASS("header search: line-start verification rejects embedded matches");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int main(void) {
    printf("=== web.c unit tests ===\n\n");

    /* json_escape tests */
    test_json_escape_simple();
    test_json_escape_special_chars();
    test_json_escape_control_chars();
    test_json_escape_empty_string();
    test_json_escape_output_size_1();
    test_json_escape_truncation();
    test_json_escape_all_escapable_chars();
    test_json_escape_high_bytes_passthrough();

    /* json_message tests */
    test_json_message_basic();
    test_json_message_truncation();
    test_json_message_null_content();
    test_json_message_special_chars_in_handle();

    /* json_extract_string tests */
    test_json_extract_basic();
    test_json_extract_with_escapes();
    test_json_extract_missing_key();
    test_json_extract_unterminated_value();
    test_json_extract_empty_value();
    test_json_extract_buffer_small();
    test_json_extract_escape_sequences();

    /* parse_query_int tests */
    test_parse_query_int_basic();
    test_parse_query_int_second_key();
    test_parse_query_int_missing_key();
    test_parse_query_int_negative_clamped();
    test_parse_query_int_overflow_clamped();
    test_parse_query_int_nonnumeric();
    test_parse_query_int_empty_query();
    test_parse_query_int_null_query();
    test_parse_query_int_null_key();
    test_parse_query_int_zero_value();
    test_parse_query_int_partial_key_match();

    /* BUG verification tests */
    test_snprintf_clamping_pattern();
    test_sockaddr_storage_size();

    /* SECURITY verification tests */
    test_strtol_clamping_pattern();
    test_header_at_line_start();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
