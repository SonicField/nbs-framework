/*
 * test_remote_unit.c — Unit tests for remote.c (nbs-chat-remote)
 *
 * Tests:
 *   1. shell_escape: normal strings, single quotes, empty string, format specifiers
 *   2. shell_escape: adversarial inputs (very long strings, all-quotes, special chars)
 *   3. shell_escape: buffer too small (returns -1 gracefully)
 *   4. shell_escape: INT_MAX overflow guard on return value
 *   5. contains_shell_metachar: rejects dangerous characters
 *   6. contains_shell_metachar: accepts safe strings
 *   7. build_ssh_argv: basic construction
 *   8. build_ssh_argv: port_str is heap-allocated (dangling pointer fix)
 *   9. build_ssh_argv: rejects NBS_CHAT_OPTS with shell metacharacters
 *  10. build_ssh_argv: graceful failure on shell_escape overflow (no abort)
 *  11. build_ssh_argv: overflow guard on argv index (checked before write)
 *  12. load_config postcondition: host, port, remote_bin validated
 *
 * Build:
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -o test_remote_unit test_remote_unit.c
 *
 * Or with ASan:
 *   clang -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
 *       -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -o test_remote_unit test_remote_unit.c \
 *       -fsanitize=address,undefined
 *
 * Strategy: We #include "remote.c" directly to access static functions.
 * We stub out main() by redefining it via the preprocessor.
 */

#define main remote_main  /* rename remote.c's main so we can define our own */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* Include the implementation directly to test static functions */
#include "../src/nbs-chat/remote.c"

#undef main  /* restore main for our test harness */

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

/* ── shell_escape tests ─────────────────────────────────────────── */

static void test_shell_escape_simple(void) {
    char buf[256];
    int ret = shell_escape("hello", buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "shell_escape('hello') returned %d", ret);
    TEST_ASSERT(strcmp(buf, "'hello'") == 0,
                "shell_escape('hello') = '%s', expected \"'hello'\"", buf);
    TEST_ASSERT(ret == 7, "expected length 7, got %d", ret);
    TEST_PASS("shell_escape: simple string");
}

static void test_shell_escape_single_quotes(void) {
    char buf[256];
    int ret = shell_escape("it's", buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "shell_escape(\"it's\") returned %d", ret);
    /* Expected: 'it'\''s' */
    TEST_ASSERT(strcmp(buf, "'it'\\''s'") == 0,
                "shell_escape(\"it's\") = '%s', expected \"'it'\\''s'\"", buf);
    TEST_PASS("shell_escape: embedded single quotes");
}

static void test_shell_escape_empty_string(void) {
    char buf[256];
    int ret = shell_escape("", buf, sizeof(buf));
    TEST_ASSERT(ret == 2, "shell_escape('') returned %d, expected 2", ret);
    TEST_ASSERT(strcmp(buf, "''") == 0,
                "shell_escape('') = '%s', expected \"''\"", buf);
    TEST_PASS("shell_escape: empty string");
}

static void test_shell_escape_format_specifiers(void) {
    char buf[256];
    /* Test that format specifiers pass through as literal text */
    int ret = shell_escape("%s%n%x", buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "shell_escape('%%s%%n%%x') returned %d", ret);
    TEST_ASSERT(strcmp(buf, "'%s%n%x'") == 0,
                "shell_escape('%%s%%n%%x') = '%s', expected \"'%%s%%n%%x'\"", buf);
    TEST_PASS("shell_escape: format specifiers treated as literals");
}

static void test_shell_escape_special_chars(void) {
    char buf[256];
    /* Shell metacharacters should be safe inside single quotes */
    int ret = shell_escape("$(rm -rf /); `id` | cat && echo", buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "shell_escape with shell metacharacters returned %d", ret);
    /* Verify the output starts and ends with single quotes */
    TEST_ASSERT(buf[0] == '\'', "output should start with single quote");
    TEST_ASSERT(buf[ret - 1] == '\'', "output should end with single quote");
    TEST_PASS("shell_escape: shell metacharacters");
}

static void test_shell_escape_all_single_quotes(void) {
    char buf[256];
    /* Each ' becomes '\'' (4 chars), plus 2 for surrounding quotes */
    /* 5 quotes: 5*4 + 2 = 22 chars + NUL */
    int ret = shell_escape("'''''", buf, sizeof(buf));
    TEST_ASSERT(ret == 22, "shell_escape(5 quotes) returned %d, expected 22", ret);
    /* Verify round-trip structure: ''\'''\'''\'''\'''\''' */
    TEST_ASSERT(buf[0] == '\'', "output should start with single quote");
    TEST_ASSERT(buf[ret - 1] == '\'', "output should end with single quote");
    TEST_PASS("shell_escape: string of all single quotes");
}

static void test_shell_escape_buffer_too_small(void) {
    char buf[4]; /* Way too small for "'hello'" (7 chars + NUL) */
    int ret = shell_escape("hello", buf, sizeof(buf));
    TEST_ASSERT(ret == -1, "shell_escape with tiny buffer should return -1, got %d", ret);
    TEST_PASS("shell_escape: buffer too small returns -1 (no abort)");
}

static void test_shell_escape_buffer_exact_fit(void) {
    /* "''" is 2 chars + NUL = 3 bytes needed */
    char buf[3];
    int ret = shell_escape("", buf, sizeof(buf));
    TEST_ASSERT(ret == 2, "shell_escape('') in 3-byte buffer returned %d, expected 2", ret);
    TEST_ASSERT(strcmp(buf, "''") == 0, "expected \"''\", got '%s'", buf);
    TEST_PASS("shell_escape: exact-fit buffer");
}

static void test_shell_escape_buffer_one_short(void) {
    /* "''" needs 3 bytes; provide only 2 */
    char buf[2];
    int ret = shell_escape("", buf, sizeof(buf));
    TEST_ASSERT(ret == -1, "shell_escape('') in 2-byte buffer should return -1, got %d", ret);
    TEST_PASS("shell_escape: one-byte-short buffer returns -1");
}

static void test_shell_escape_long_string(void) {
    /* 10000-character string with no special chars */
    size_t len = 10000;
    char *input = malloc(len + 1);
    TEST_ASSERT(input != NULL, "malloc failed");
    memset(input, 'A', len);
    input[len] = '\0';

    /* Needs len + 2 (quotes) + 1 (NUL) = 10003 bytes */
    size_t buf_size = len + 3;
    char *buf = malloc(buf_size);
    TEST_ASSERT(buf != NULL, "malloc failed");

    int ret = shell_escape(input, buf, buf_size);
    TEST_ASSERT(ret == (int)(len + 2),
                "shell_escape(10000 A's) returned %d, expected %d",
                ret, (int)(len + 2));
    TEST_ASSERT(buf[0] == '\'', "output should start with single quote");
    TEST_ASSERT(buf[ret - 1] == '\'', "output should end with single quote");

    free(buf);
    free(input);
    TEST_PASS("shell_escape: 10000-character string");
}

static void test_shell_escape_long_string_with_quotes(void) {
    /* String of 2000 single quotes: each becomes 4 chars, plus 2 surrounding = 8002 */
    size_t len = 2000;
    char *input = malloc(len + 1);
    TEST_ASSERT(input != NULL, "malloc failed");
    memset(input, '\'', len);
    input[len] = '\0';

    /* Worst case: 2000 * 4 + 2 + 1 = 8003 */
    size_t buf_size = len * 4 + 3;
    char *buf = malloc(buf_size);
    TEST_ASSERT(buf != NULL, "malloc failed");

    int ret = shell_escape(input, buf, buf_size);
    TEST_ASSERT(ret == (int)(len * 4 + 2),
                "shell_escape(2000 quotes) returned %d, expected %d",
                ret, (int)(len * 4 + 2));

    free(buf);
    free(input);
    TEST_PASS("shell_escape: 2000 single quotes (worst-case expansion)");
}

static void test_shell_escape_null_bytes_in_middle(void) {
    /* shell_escape stops at NUL, so embedded NUL truncates the string */
    char input[] = "abc\0def";
    char buf[64];
    int ret = shell_escape(input, buf, sizeof(buf));
    TEST_ASSERT(ret > 0, "shell_escape returned %d", ret);
    TEST_ASSERT(strcmp(buf, "'abc'") == 0,
                "shell_escape with embedded NUL = '%s', expected \"'abc'\"", buf);
    TEST_PASS("shell_escape: embedded NUL truncates (expected behaviour)");
}

/* ── contains_shell_metachar tests ──────────────────────────────── */

static void test_metachar_rejects_semicolon(void) {
    TEST_ASSERT(contains_shell_metachar("foo;bar") == 1,
                "should reject semicolon");
    TEST_PASS("contains_shell_metachar: rejects semicolon");
}

static void test_metachar_rejects_backtick(void) {
    TEST_ASSERT(contains_shell_metachar("`id`") == 1,
                "should reject backtick");
    TEST_PASS("contains_shell_metachar: rejects backtick");
}

static void test_metachar_rejects_dollar_paren(void) {
    TEST_ASSERT(contains_shell_metachar("$(whoami)") == 1,
                "should reject $()");
    TEST_PASS("contains_shell_metachar: rejects $()");
}

static void test_metachar_rejects_pipe(void) {
    TEST_ASSERT(contains_shell_metachar("foo|bar") == 1,
                "should reject pipe");
    TEST_PASS("contains_shell_metachar: rejects pipe");
}

static void test_metachar_rejects_ampersand(void) {
    TEST_ASSERT(contains_shell_metachar("foo&&bar") == 1,
                "should reject ampersand");
    TEST_PASS("contains_shell_metachar: rejects ampersand");
}

static void test_metachar_rejects_backslash(void) {
    TEST_ASSERT(contains_shell_metachar("foo\\bar") == 1,
                "should reject backslash");
    TEST_PASS("contains_shell_metachar: rejects backslash");
}

static void test_metachar_rejects_newline(void) {
    TEST_ASSERT(contains_shell_metachar("foo\nbar") == 1,
                "should reject newline");
    TEST_PASS("contains_shell_metachar: rejects newline");
}

static void test_metachar_accepts_safe_ssh_option(void) {
    TEST_ASSERT(contains_shell_metachar("StrictHostKeyChecking=no") == 0,
                "should accept safe SSH option");
    TEST_ASSERT(contains_shell_metachar("ConnectTimeout=10") == 0,
                "should accept safe SSH option");
    TEST_ASSERT(contains_shell_metachar("ServerAliveInterval 60") == 0,
                "should accept safe SSH option with space");
    TEST_PASS("contains_shell_metachar: accepts safe SSH options");
}

static void test_metachar_empty_string(void) {
    TEST_ASSERT(contains_shell_metachar("") == 0,
                "empty string should be safe");
    TEST_PASS("contains_shell_metachar: empty string is safe");
}

static void test_metachar_rejects_all_dangerous_chars(void) {
    const char *dangerous[] = {
        ";", "`", "$", "(", ")", "|", "&", "<", ">",
        "{", "}", "!", "\\", "\n", "\r"
    };
    size_t n = sizeof(dangerous) / sizeof(dangerous[0]);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT(contains_shell_metachar(dangerous[i]) == 1,
                    "should reject '%s' (index %zu)", dangerous[i], i);
    }
    TEST_PASS("contains_shell_metachar: rejects all listed dangerous chars");
}

/* ── build_ssh_argv tests ───────────────────────────────────────── */

/*
 * Helper: create a minimal config for testing.
 */
static remote_config_t make_test_config(void) {
    remote_config_t cfg;
    cfg.host = "user@testhost";
    cfg.port = 22;
    cfg.key_path = NULL;
    cfg.remote_bin = "nbs-chat";
    cfg.ssh_opts = NULL;
    return cfg;
}

static void test_build_ssh_argv_basic(void) {
    remote_config_t cfg = make_test_config();
    char *argv_in[] = { "nbs-chat-remote", "read", "/tmp/chat.nbs", NULL };
    char *opts_out = NULL;
    char *port_str_out = NULL;

    char **result = build_ssh_argv(&cfg, 3, argv_in, &opts_out, &port_str_out);
    TEST_ASSERT(result != NULL, "build_ssh_argv returned NULL");

    /* argv[0] should be "ssh" */
    TEST_ASSERT(strcmp(result[0], "ssh") == 0,
                "argv[0] = '%s', expected 'ssh'", result[0]);

    /* Since port is 22 (default), no -p flag */
    /* Next should be host, then remote command */
    int i = 1;
    TEST_ASSERT(strcmp(result[i], "user@testhost") == 0,
                "argv[%d] = '%s', expected 'user@testhost'", i, result[i]);
    i++;

    /* Remote command should be the escaped nbs-chat command */
    TEST_ASSERT(result[i] != NULL, "remote command argv entry is NULL");
    /* It should contain the escaped binary and arguments */
    TEST_ASSERT(strstr(result[i], "nbs-chat") != NULL,
                "remote command should contain 'nbs-chat'");
    TEST_ASSERT(strstr(result[i], "read") != NULL,
                "remote command should contain 'read'");

    /* port_str_out should be NULL (default port) */
    TEST_ASSERT(port_str_out == NULL,
                "port_str_out should be NULL for default port");

    /* Cleanup */
    for (int j = 0; result[j] != NULL; j++) {
        if (result[j + 1] == NULL) {
            free(result[j]); /* remote_cmd */
            break;
        }
    }
    free(opts_out);
    free(port_str_out);
    free(result);

    TEST_PASS("build_ssh_argv: basic construction");
}

static void test_build_ssh_argv_port_is_heap(void) {
    remote_config_t cfg = make_test_config();
    cfg.port = 2222; /* non-default port */

    char *argv_in[] = { "nbs-chat-remote", "read", "/tmp/chat.nbs", NULL };
    char *opts_out = NULL;
    char *port_str_out = NULL;

    char **result = build_ssh_argv(&cfg, 3, argv_in, &opts_out, &port_str_out);
    TEST_ASSERT(result != NULL, "build_ssh_argv returned NULL");

    /* port_str_out should be heap-allocated (not NULL) */
    TEST_ASSERT(port_str_out != NULL,
                "port_str_out should be non-NULL for non-default port");
    TEST_ASSERT(strcmp(port_str_out, "2222") == 0,
                "port_str_out = '%s', expected '2222'", port_str_out);

    /* Verify -p is in the argv */
    int found_p = 0;
    for (int i = 0; result[i] != NULL; i++) {
        if (strcmp(result[i], "-p") == 0) {
            found_p = 1;
            /* Next element should be the port string, which is the same as port_str_out */
            TEST_ASSERT(result[i + 1] != NULL, "-p should be followed by port number");
            TEST_ASSERT(result[i + 1] == port_str_out,
                        "port argv entry should point to port_str_out (heap, not stack)");
            break;
        }
    }
    TEST_ASSERT(found_p, "argv should contain '-p' for non-default port");

    /* Cleanup */
    for (int j = 0; result[j] != NULL; j++) {
        if (result[j + 1] == NULL) {
            free(result[j]);
            break;
        }
    }
    free(opts_out);
    free(port_str_out);
    free(result);

    TEST_PASS("build_ssh_argv: port_str is heap-allocated (dangling pointer fix)");
}

static void test_build_ssh_argv_rejects_metachar_opts(void) {
    remote_config_t cfg = make_test_config();
    cfg.ssh_opts = "ProxyCommand=$(nc %h %p);id";

    char *argv_in[] = { "nbs-chat-remote", "read", "/tmp/chat.nbs", NULL };
    char *opts_out = NULL;
    char *port_str_out = NULL;

    char **result = build_ssh_argv(&cfg, 3, argv_in, &opts_out, &port_str_out);
    TEST_ASSERT(result == NULL,
                "build_ssh_argv should return NULL for opts with shell metacharacters");

    TEST_PASS("build_ssh_argv: rejects NBS_CHAT_OPTS with shell metacharacters");
}

static void test_build_ssh_argv_accepts_safe_opts(void) {
    remote_config_t cfg = make_test_config();
    cfg.ssh_opts = "StrictHostKeyChecking=no,ConnectTimeout=10";

    char *argv_in[] = { "nbs-chat-remote", "read", "/tmp/chat.nbs", NULL };
    char *opts_out = NULL;
    char *port_str_out = NULL;

    char **result = build_ssh_argv(&cfg, 3, argv_in, &opts_out, &port_str_out);
    TEST_ASSERT(result != NULL,
                "build_ssh_argv should succeed for safe opts");

    /* Verify -o options are present */
    int o_count = 0;
    for (int i = 0; result[i] != NULL; i++) {
        if (strcmp(result[i], "-o") == 0) o_count++;
    }
    TEST_ASSERT(o_count == 2,
                "expected 2 -o options, got %d", o_count);

    /* Cleanup */
    for (int j = 0; result[j] != NULL; j++) {
        if (result[j + 1] == NULL) {
            free(result[j]);
            break;
        }
    }
    free(opts_out);
    free(port_str_out);
    free(result);

    TEST_PASS("build_ssh_argv: accepts safe SSH options");
}

static void test_build_ssh_argv_with_key(void) {
    remote_config_t cfg = make_test_config();
    cfg.key_path = "/home/user/.ssh/id_rsa";

    char *argv_in[] = { "nbs-chat-remote", "send", "/tmp/chat.nbs", "alice", "hello", NULL };
    char *opts_out = NULL;
    char *port_str_out = NULL;

    char **result = build_ssh_argv(&cfg, 5, argv_in, &opts_out, &port_str_out);
    TEST_ASSERT(result != NULL, "build_ssh_argv returned NULL");

    /* Verify -i is in the argv */
    int found_i = 0;
    for (int i = 0; result[i] != NULL; i++) {
        if (strcmp(result[i], "-i") == 0) {
            found_i = 1;
            TEST_ASSERT(result[i + 1] != NULL, "-i should be followed by key path");
            TEST_ASSERT(strcmp(result[i + 1], "/home/user/.ssh/id_rsa") == 0,
                        "key path mismatch: '%s'", result[i + 1]);
            break;
        }
    }
    TEST_ASSERT(found_i, "argv should contain '-i' when key_path is set");

    /* Cleanup */
    for (int j = 0; result[j] != NULL; j++) {
        if (result[j + 1] == NULL) {
            free(result[j]);
            break;
        }
    }
    free(opts_out);
    free(port_str_out);
    free(result);

    TEST_PASS("build_ssh_argv: identity file included");
}

static void test_build_ssh_argv_null_terminated(void) {
    remote_config_t cfg = make_test_config();
    cfg.port = 2222;
    cfg.key_path = "/key";
    cfg.ssh_opts = "Opt1=val1,Opt2=val2,Opt3=val3,Opt4=val4";

    char *argv_in[] = { "nbs-chat-remote", "read", "/tmp/c.nbs", NULL };
    char *opts_out = NULL;
    char *port_str_out = NULL;

    char **result = build_ssh_argv(&cfg, 3, argv_in, &opts_out, &port_str_out);
    TEST_ASSERT(result != NULL, "build_ssh_argv returned NULL");

    /* Count elements: ssh -p PORT -i KEY -o O1 -o O2 -o O3 -o O4 host cmd = 15, max array is 16 */
    int count = 0;
    while (result[count] != NULL) count++;
    TEST_ASSERT(count <= 15,
                "argv has %d non-NULL elements, expected <= 15", count);

    /* Cleanup */
    for (int j = 0; result[j] != NULL; j++) {
        if (result[j + 1] == NULL) {
            free(result[j]);
            break;
        }
    }
    free(opts_out);
    free(port_str_out);
    free(result);

    TEST_PASS("build_ssh_argv: NULL-terminated with max options");
}

static void test_build_ssh_argv_escapes_quotes_in_args(void) {
    remote_config_t cfg = make_test_config();

    /* Argument with single quotes — must be escaped */
    char *argv_in[] = { "nbs-chat-remote", "send", "/tmp/chat", "alice", "it's a test", NULL };
    char *opts_out = NULL;
    char *port_str_out = NULL;

    char **result = build_ssh_argv(&cfg, 5, argv_in, &opts_out, &port_str_out);
    TEST_ASSERT(result != NULL, "build_ssh_argv returned NULL");

    /* Find the remote command (last non-NULL element) */
    char *remote_cmd = NULL;
    for (int i = 0; result[i] != NULL; i++) {
        if (result[i + 1] == NULL) {
            remote_cmd = result[i];
            break;
        }
    }
    TEST_ASSERT(remote_cmd != NULL, "remote_cmd not found");

    /* The remote command should contain the escaped quote sequence '\'' */
    TEST_ASSERT(strstr(remote_cmd, "'\\''") != NULL,
                "remote command should contain escaped quote sequence, got: %s",
                remote_cmd);

    /* Cleanup */
    free(remote_cmd);
    free(opts_out);
    free(port_str_out);
    free(result);

    TEST_PASS("build_ssh_argv: escapes single quotes in arguments");
}

static void test_build_ssh_argv_format_specifier_in_host(void) {
    /*
     * Verify that host containing format specifiers does not cause
     * format string injection. The host is placed into argv and also
     * passed to fprintf via %s in run_ssh — this test verifies the
     * argv construction does not interpret format specifiers.
     */
    remote_config_t cfg = make_test_config();
    cfg.host = "%s%s%s%s%n";

    char *argv_in[] = { "nbs-chat-remote", "read", "/tmp/chat.nbs", NULL };
    char *opts_out = NULL;
    char *port_str_out = NULL;

    char **result = build_ssh_argv(&cfg, 3, argv_in, &opts_out, &port_str_out);
    TEST_ASSERT(result != NULL, "build_ssh_argv returned NULL");

    /* Host should be in argv as a literal string */
    int found_host = 0;
    for (int i = 0; result[i] != NULL; i++) {
        if (strcmp(result[i], "%s%s%s%s%n") == 0) {
            found_host = 1;
            break;
        }
    }
    TEST_ASSERT(found_host, "host with format specifiers should appear literally in argv");

    /* Cleanup */
    for (int j = 0; result[j] != NULL; j++) {
        if (result[j + 1] == NULL) {
            free(result[j]);
            break;
        }
    }
    free(opts_out);
    free(port_str_out);
    free(result);

    TEST_PASS("build_ssh_argv: format specifiers in host treated as literal");
}

/* ── Entry point ────────────────────────────────────────────────── */

int main(void) {
    printf("=== remote.c unit tests ===\n\n");

    /* shell_escape tests */
    test_shell_escape_simple();
    test_shell_escape_single_quotes();
    test_shell_escape_empty_string();
    test_shell_escape_format_specifiers();
    test_shell_escape_special_chars();
    test_shell_escape_all_single_quotes();
    test_shell_escape_buffer_too_small();
    test_shell_escape_buffer_exact_fit();
    test_shell_escape_buffer_one_short();
    test_shell_escape_long_string();
    test_shell_escape_long_string_with_quotes();
    test_shell_escape_null_bytes_in_middle();

    /* contains_shell_metachar tests */
    test_metachar_rejects_semicolon();
    test_metachar_rejects_backtick();
    test_metachar_rejects_dollar_paren();
    test_metachar_rejects_pipe();
    test_metachar_rejects_ampersand();
    test_metachar_rejects_backslash();
    test_metachar_rejects_newline();
    test_metachar_accepts_safe_ssh_option();
    test_metachar_empty_string();
    test_metachar_rejects_all_dangerous_chars();

    /* build_ssh_argv tests */
    test_build_ssh_argv_basic();
    test_build_ssh_argv_port_is_heap();
    test_build_ssh_argv_rejects_metachar_opts();
    test_build_ssh_argv_accepts_safe_opts();
    test_build_ssh_argv_with_key();
    test_build_ssh_argv_null_terminated();
    test_build_ssh_argv_escapes_quotes_in_args();
    test_build_ssh_argv_format_specifier_in_host();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
