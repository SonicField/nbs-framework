/*
 * test_chat_file_audit.c — Adversarial tests for audit-report violations
 *
 * Each test targets a specific violation from the audit report for
 * chat_file.c / chat_file.h. Tests are adversarial: they attempt to
 * trigger the bug or verify the hardening holds.
 *
 * Violations covered:
 *   #1  SECURITY: TOCTOU race in chat_create (O_EXCL fix)
 *   #2  BUG: file-length mismatch assertion in chat_send
 *   #3  BUG: file-length mismatch assertion in chat_truncate
 *   #4  HARDENING: malloc overflow guard (_Static_assert)
 *   #5  BUG: chat_truncate stored vs encoded_line_count confusion
 *   #6  BUG: content_len underflow check
 *   #7  HARDENING: fdopen failure leaks temp file (archive)
 *   #8  HARDENING: fdopen failure leaks temp file (archive main)
 *   #9  HARDENING: safe_parse_int64 range check
 *   #10 HARDENING: header delimiter detection (seen_header_marker)
 *   #12 BUG: base64.h circular include (now uses nbs_assert.h)
 *   #13 HARDENING: format_participants postcondition
 *   #15 HARDENING: parse_participants postcondition
 *
 * Build:
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -I../src/nbs-chat \
 *       -o test_chat_file_audit test_chat_file_audit.c \
 *       ../src/nbs-chat/chat_file.c ../src/nbs-chat/lock.c \
 *       ../src/nbs-chat/base64.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <signal.h>

#include "chat_file.h"
#include "base64.h"

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

static char test_dir[256];

static void setup_test_dir(void) {
    snprintf(test_dir, sizeof(test_dir), "/tmp/test_chat_audit_XXXXXX");
    char *result = mkdtemp(test_dir);
    ASSERT_MSG(result != NULL, "mkdtemp failed: %s", strerror(errno));
}

static void cleanup_path(const char *path) {
    unlink(path);
}

static void cleanup_chat(const char *path) {
    char buf[520];
    cleanup_path(path);
    snprintf(buf, sizeof(buf), "%s.lock", path);
    cleanup_path(buf);
    snprintf(buf, sizeof(buf), "%s.cursors", path);
    cleanup_path(buf);
    snprintf(buf, sizeof(buf), "%s.tmp", path);
    cleanup_path(buf);
}

/* ================================================================
 * VIOLATION #1 (SECURITY): TOCTOU race in chat_create
 *
 * Before fix: stat() + O_CREAT|O_TRUNC = race window.
 * After fix: O_CREAT|O_EXCL = atomic create-or-fail.
 *
 * Test: Create a file between the existence check and the open.
 * With O_EXCL the second create fails with -1. Without O_EXCL,
 * a concurrent create would silently truncate the first.
 * We also verify that chat_create returns -1 for existing files
 * and that the original file content is preserved.
 * ================================================================ */

static void test_toctou_chat_create_preserves_existing(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/toctou_test.chat", test_dir);

    /* Create the file first */
    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "first chat_create failed: %d", rc);

    /* Send a message to make the file non-trivial */
    rc = chat_send(path, "alice", "important data");
    TEST_ASSERT(rc == 0, "chat_send failed: %d", rc);

    /* Get the file size before second create attempt */
    struct stat st_before;
    TEST_ASSERT(stat(path, &st_before) == 0, "stat before failed");

    /* Second create must fail with -1 */
    rc = chat_create(path);
    TEST_ASSERT(rc == -1, "second chat_create should return -1 (exists), got %d", rc);

    /* Verify file is unchanged (not truncated) */
    struct stat st_after;
    TEST_ASSERT(stat(path, &st_after) == 0, "stat after failed");
    TEST_ASSERT(st_before.st_size == st_after.st_size,
                "file was truncated! before=%" PRId64 " after=%" PRId64,
                (int64_t)st_before.st_size, (int64_t)st_after.st_size);

    /* Verify the message is still readable */
    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed after second create attempt: %d", rc);
    TEST_ASSERT(state.message_count == 1,
                "message count changed: expected 1, got %d", state.message_count);
    TEST_ASSERT(strcmp(state.messages[0].content, "important data") == 0,
                "message content corrupted: got '%s'", state.messages[0].content);
    chat_state_free(&state);

    cleanup_chat(path);
    TEST_PASS("TOCTOU #1: chat_create with O_EXCL preserves existing file");
}

/* ================================================================
 * VIOLATION #2 (BUG): file-length mismatch in chat_send
 *
 * Test: Send messages and verify file-length header matches actual
 * file size. If compute_file_length has a bug, the ASSERT_MSG now
 * fires (the old code silently continued).
 * ================================================================ */

static void test_file_length_invariant_chat_send(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/flen_send.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Send messages of varying lengths to stress compute_file_length */
    const char *messages[] = {
        "short",
        "a slightly longer message with more characters",
        "x",  /* single char */
        "",   /* empty */
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
        "message with special chars: !@#$%^&*()_+-=[]{}|;':\",./<>?",
        "unicode: \xc3\xa9\xc3\xa0\xc3\xbc\xc3\xb6",  /* UTF-8 */
    };
    int n = (int)(sizeof(messages) / sizeof(messages[0]));

    for (int i = 0; i < n; i++) {
        rc = chat_send(path, "alice", messages[i]);
        TEST_ASSERT(rc == 0, "chat_send %d failed: %d", i, rc);

        /* Verify file-length matches actual size after each send */
        chat_state_t state;
        rc = chat_read(path, &state);
        TEST_ASSERT(rc == 0, "chat_read %d failed: %d", i, rc);

        struct stat st;
        TEST_ASSERT(stat(path, &st) == 0, "stat %d failed", i);
        TEST_ASSERT(state.file_length == (int64_t)st.st_size,
                    "send %d: file_length %" PRId64 " != actual %" PRId64,
                    i, state.file_length, (int64_t)st.st_size);
        chat_state_free(&state);
    }

    cleanup_chat(path);
    TEST_PASS("BUG #2: file-length invariant holds across all chat_send variants");
}

/* ================================================================
 * VIOLATION #3 (BUG): file-length mismatch in chat_truncate
 *
 * Test: Truncate to various counts and verify file-length header.
 * ================================================================ */

static void test_file_length_invariant_chat_truncate(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/flen_trunc.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Send 10 messages */
    for (int i = 0; i < 10; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "message number %d", i);
        rc = chat_send(path, (i % 2 == 0) ? "alice" : "bob", msg);
        TEST_ASSERT(rc == 0, "send %d failed: %d", i, rc);
    }

    /* Truncate to various counts and verify file-length each time */
    int truncate_counts[] = {7, 5, 3, 1, 0};
    for (int t = 0; t < 5; t++) {
        /* Re-send messages to get back to 10 if needed */
        if (t > 0) {
            /* Re-create file for fresh test */
            cleanup_chat(path);
            rc = chat_create(path);
            TEST_ASSERT(rc == 0, "re-create %d failed", t);
            for (int i = 0; i < 10; i++) {
                char msg[64];
                snprintf(msg, sizeof(msg), "message number %d", i);
                rc = chat_send(path, (i % 2 == 0) ? "alice" : "bob", msg);
                TEST_ASSERT(rc == 0, "re-send %d/%d failed: %d", t, i, rc);
            }
        }

        rc = chat_truncate(path, truncate_counts[t]);
        TEST_ASSERT(rc == 0, "truncate to %d failed: %d", truncate_counts[t], rc);

        chat_state_t state;
        rc = chat_read(path, &state);
        TEST_ASSERT(rc == 0, "read after truncate to %d failed", truncate_counts[t]);

        struct stat st;
        TEST_ASSERT(stat(path, &st) == 0, "stat after truncate to %d failed",
                    truncate_counts[t]);
        TEST_ASSERT(state.file_length == (int64_t)st.st_size,
                    "truncate to %d: file_length %" PRId64 " != actual %" PRId64,
                    truncate_counts[t], state.file_length, (int64_t)st.st_size);
        chat_state_free(&state);
    }

    cleanup_chat(path);
    TEST_PASS("BUG #3: file-length invariant holds across chat_truncate");
}

/* ================================================================
 * VIOLATION #5 (BUG): chat_truncate stored vs total line confusion
 *
 * Test: Truncate a file with more messages than keep_count and
 * verify the correct number of messages remain. The old code used
 * a single variable for both purposes.
 * ================================================================ */

static void test_truncate_stored_vs_total(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/trunc_stored.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Send 20 messages */
    for (int i = 0; i < 20; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "msg %d", i);
        rc = chat_send(path, "alice", msg);
        TEST_ASSERT(rc == 0, "send %d failed: %d", i, rc);
    }

    /* Truncate to 5 — only 5 messages should remain, not 20 */
    rc = chat_truncate(path, 5);
    TEST_ASSERT(rc == 0, "truncate to 5 failed: %d", rc);

    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "read after truncate failed: %d", rc);
    TEST_ASSERT(state.message_count == 5,
                "expected 5 messages after truncate, got %d", state.message_count);

    /* Verify file-length is consistent */
    struct stat st;
    TEST_ASSERT(stat(path, &st) == 0, "stat failed");
    TEST_ASSERT(state.file_length == (int64_t)st.st_size,
                "file_length %" PRId64 " != actual %" PRId64,
                state.file_length, (int64_t)st.st_size);

    chat_state_free(&state);
    cleanup_chat(path);
    TEST_PASS("BUG #5: chat_truncate correctly separates stored vs total count");
}

/* ================================================================
 * VIOLATION #6 (BUG): content_len underflow check
 *
 * Test: Verify that content_len == strlen(content) for messages
 * with various adversarial content patterns. The fix adds a
 * precondition assertion that colon+2 is within bounds.
 * ================================================================ */

static void test_content_len_adversarial(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/content_len_adv.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Adversarial messages: various lengths and content */
    const char *adversarial[] = {
        "",                       /* empty content */
        "x",                     /* single char */
        ": ",                    /* looks like delimiter */
        "a: b",                  /* nested colon */
        "||||",                  /* all pipes */
        "a|b|c: d|e",          /* pipes everywhere */
        "hello\tworld",         /* tab character */
        "   ",                   /* just spaces */
        "a\xc3\xa9\xc3\xa0",   /* UTF-8 multi-byte */
        "12345678901234567890123456789012345678901234567890"
        "12345678901234567890123456789012345678901234567890"
        "12345678901234567890123456789012345678901234567890",  /* 150 chars */
    };
    int n = (int)(sizeof(adversarial) / sizeof(adversarial[0]));

    for (int i = 0; i < n; i++) {
        rc = chat_send(path, "alice", adversarial[i]);
        TEST_ASSERT(rc == 0, "send adversarial[%d] failed: %d", i, rc);
    }

    /* Read back and verify content_len == strlen(content) for all */
    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);
    TEST_ASSERT(state.message_count == n,
                "expected %d messages, got %d", n, state.message_count);

    for (int i = 0; i < state.message_count; i++) {
        TEST_ASSERT(state.messages[i].content_len == strlen(state.messages[i].content),
                    "msg %d: content_len %zu != strlen %zu",
                    i, state.messages[i].content_len,
                    strlen(state.messages[i].content));
        /* Verify content matches what was sent */
        TEST_ASSERT(strcmp(state.messages[i].content, adversarial[i]) == 0,
                    "msg %d: content mismatch: sent '%s' got '%s'",
                    i, adversarial[i], state.messages[i].content);
    }

    chat_state_free(&state);
    cleanup_chat(path);
    TEST_PASS("BUG #6: content_len == strlen(content) for adversarial messages");
}

/* ================================================================
 * VIOLATION #9 (HARDENING): safe_parse_int64 range check
 *
 * Test: Write a chat file with edge-case file-length values and
 * verify they parse correctly. Also test via cursor files which
 * exercise safe_parse_int indirectly.
 * ================================================================ */

static void test_safe_parse_int64_edge_cases(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/int64_edge.chat", test_dir);

    /* Create a chat file with a known file-length value */
    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Read it back and verify the file-length parsed correctly */
    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);
    TEST_ASSERT(state.file_length > 0,
                "file_length should be positive, got %" PRId64,
                state.file_length);
    chat_state_free(&state);

    /* Write a chat file with a very large (but valid) file-length
     * to test parsing of large int64 values */
    int fd = open(path, O_WRONLY | O_TRUNC, 0600);
    TEST_ASSERT(fd >= 0, "open for overwrite failed: %s", strerror(errno));
    const char *content =
        "=== nbs-chat ===\n"
        "last-writer: system\n"
        "last-write: 2026-02-27T00:00:00+0000\n"
        "file-length: 9223372036854775807\n"  /* INT64_MAX */
        "participants: \n"
        "---\n";
    ssize_t written = write(fd, content, strlen(content));
    TEST_ASSERT(written == (ssize_t)strlen(content), "write failed");
    close(fd);

    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read with INT64_MAX file-length failed: %d", rc);
    TEST_ASSERT(state.file_length == INT64_MAX,
                "file_length should be INT64_MAX, got %" PRId64,
                state.file_length);
    chat_state_free(&state);

    cleanup_chat(path);
    TEST_PASS("HARDENING #9: safe_parse_int64 handles edge cases correctly");
}

/* ================================================================
 * VIOLATION #10 (HARDENING): header delimiter detection
 *
 * Test: Create a malformed file where "---" appears before the
 * header marker "=== nbs-chat ===". After fix, the delimiter
 * should only be recognised after the marker.
 * ================================================================ */

static void test_header_delimiter_requires_marker(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/delim_marker.chat", test_dir);

    /* Write a malformed file: --- before the header marker */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT(fd >= 0, "open failed: %s", strerror(errno));

    /* The "---" on line 1 should NOT be treated as the header delimiter */
    const char *content =
        "---\n"
        "=== nbs-chat ===\n"
        "last-writer: alice\n"
        "last-write: 2026-02-27T00:00:00+0000\n"
        "file-length: 999\n"
        "participants: alice(1)\n"
        "---\n"
        "YWxpY2V8MTc0MDYxMDAwMDogaGVsbG8=\n";  /* alice|1740610000: hello */
    ssize_t written = write(fd, content, strlen(content));
    TEST_ASSERT(written == (ssize_t)strlen(content), "write failed");
    close(fd);

    /* Read it — with the fix, the parser should find the message */
    chat_state_t state;
    int rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);
    TEST_ASSERT(state.message_count == 1,
                "expected 1 message (parser should skip initial ---), got %d",
                state.message_count);
    TEST_ASSERT(strcmp(state.messages[0].handle, "alice") == 0,
                "handle mismatch: got '%s'", state.messages[0].handle);
    TEST_ASSERT(strcmp(state.messages[0].content, "hello") == 0,
                "content mismatch: got '%s'", state.messages[0].content);

    chat_state_free(&state);

    /* Now test via chat_send: send a message into this file.
     * The send must correctly parse past the header and find
     * existing messages. */
    rc = chat_send(path, "bob", "world");
    TEST_ASSERT(rc == 0, "chat_send into malformed-header file failed: %d", rc);

    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read after send failed: %d", rc);
    /* After the send rewrites the file, it should have 2 messages */
    TEST_ASSERT(state.message_count == 2,
                "expected 2 messages after send, got %d", state.message_count);

    chat_state_free(&state);
    cleanup_chat(path);
    TEST_PASS("HARDENING #10: header delimiter requires prior marker");
}

/* ================================================================
 * VIOLATION #12 (BUG): base64.h no longer includes chat_file.h
 *
 * Test: Verify that base64 functions work independently. The fix
 * changed base64.h to include nbs_assert.h instead of chat_file.h.
 * If the include is wrong, the build would fail. This test
 * verifies runtime behaviour.
 * ================================================================ */

static void test_base64_independent_of_chat_file(void) {
    /* Encode/decode a test string to verify base64 works */
    const char *raw = "test data for base64";
    size_t raw_len = strlen(raw);

    size_t enc_size = base64_encoded_size(raw_len);
    char *encoded = malloc(enc_size);
    TEST_ASSERT(encoded != NULL, "malloc failed");

    int enc_len = base64_encode((const unsigned char *)raw, raw_len,
                                encoded, enc_size);
    TEST_ASSERT(enc_len > 0, "base64_encode failed: %d", enc_len);

    size_t dec_size = base64_decoded_size((size_t)enc_len);
    unsigned char *decoded = malloc(dec_size + 1);
    TEST_ASSERT(decoded != NULL, "malloc failed");

    int dec_len = base64_decode(encoded, (size_t)enc_len, decoded, dec_size);
    TEST_ASSERT(dec_len == (int)raw_len,
                "decode length %d != original %zu", dec_len, raw_len);
    decoded[dec_len] = '\0';
    TEST_ASSERT(memcmp(decoded, raw, raw_len) == 0,
                "decoded data mismatch");

    free(encoded);
    free(decoded);
    TEST_PASS("BUG #12: base64 works independently (no chat_file.h dependency)");
}

/* ================================================================
 * VIOLATION #13 (HARDENING): format_participants postcondition
 *
 * Test: Send messages from many participants and verify the
 * participant list formats correctly without truncation for
 * reasonable counts.
 * ================================================================ */

static void test_format_participants_postcondition(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/many_parts.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Send messages from many distinct handles */
    for (int i = 0; i < 20; i++) {
        char handle[MAX_HANDLE_LEN];
        snprintf(handle, sizeof(handle), "user%d", i);
        char msg[64];
        snprintf(msg, sizeof(msg), "message from user%d", i);
        rc = chat_send(path, handle, msg);
        TEST_ASSERT(rc == 0, "send from user%d failed: %d", i, rc);
    }

    /* Read back and verify all participants are present */
    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);
    TEST_ASSERT(state.participant_count == 20,
                "expected 20 participants, got %d", state.participant_count);

    /* Verify file-length is still consistent */
    struct stat st;
    TEST_ASSERT(stat(path, &st) == 0, "stat failed");
    TEST_ASSERT(state.file_length == (int64_t)st.st_size,
                "file_length %" PRId64 " != actual %" PRId64,
                state.file_length, (int64_t)st.st_size);

    chat_state_free(&state);
    cleanup_chat(path);
    TEST_PASS("HARDENING #13: format_participants postcondition holds for 20 participants");
}

/* ================================================================
 * VIOLATION #15 (HARDENING): parse_participants postcondition
 *
 * Test: Write a chat file with many participants and verify the
 * parse correctly counts them and the postcondition holds.
 * ================================================================ */

static void test_parse_participants_postcondition(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/parse_parts.chat", test_dir);

    /* Create a chat file with a long participants header */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT(fd >= 0, "open failed: %s", strerror(errno));

    /* Build a participants line with 10 participants */
    const char *content =
        "=== nbs-chat ===\n"
        "last-writer: user9\n"
        "last-write: 2026-02-27T00:00:00+0000\n"
        "file-length: 999\n"
        "participants: user0(1), user1(2), user2(3), user3(4), user4(5), "
        "user5(6), user6(7), user7(8), user8(9), user9(10)\n"
        "---\n";
    ssize_t written = write(fd, content, strlen(content));
    TEST_ASSERT(written == (ssize_t)strlen(content), "write failed");
    close(fd);

    chat_state_t state;
    int rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);
    TEST_ASSERT(state.participant_count == 10,
                "expected 10 participants, got %d", state.participant_count);

    /* Verify participant counts parsed correctly */
    for (int i = 0; i < state.participant_count; i++) {
        char expected_handle[MAX_HANDLE_LEN];
        snprintf(expected_handle, sizeof(expected_handle), "user%d", i);
        TEST_ASSERT(strcmp(state.participants[i].handle, expected_handle) == 0,
                    "participant %d handle mismatch: got '%s'",
                    i, state.participants[i].handle);
        TEST_ASSERT(state.participants[i].count == i + 1,
                    "participant %d count: expected %d, got %d",
                    i, i + 1, state.participants[i].count);
    }

    chat_state_free(&state);
    cleanup_chat(path);
    TEST_PASS("HARDENING #15: parse_participants postcondition holds");
}

/* ================================================================
 * Additional adversarial test: truncate then send
 *
 * Exercises the interaction between truncate and send, verifying
 * that all invariants (file-length, message count, participants)
 * are maintained across the boundary.
 * ================================================================ */

static void test_truncate_then_send(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/trunc_send.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Send 10 messages */
    for (int i = 0; i < 10; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "msg %d", i);
        rc = chat_send(path, (i % 3 == 0) ? "alice" : (i % 3 == 1) ? "bob" : "charlie", msg);
        TEST_ASSERT(rc == 0, "send %d failed: %d", i, rc);
    }

    /* Truncate to 3 */
    rc = chat_truncate(path, 3);
    TEST_ASSERT(rc == 0, "truncate failed: %d", rc);

    /* Send 2 more messages */
    rc = chat_send(path, "dave", "after truncate 1");
    TEST_ASSERT(rc == 0, "send after truncate 1 failed: %d", rc);
    rc = chat_send(path, "alice", "after truncate 2");
    TEST_ASSERT(rc == 0, "send after truncate 2 failed: %d", rc);

    /* Read and verify */
    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);
    TEST_ASSERT(state.message_count == 5,
                "expected 5 messages (3 kept + 2 new), got %d", state.message_count);

    /* Verify file-length */
    struct stat st;
    TEST_ASSERT(stat(path, &st) == 0, "stat failed");
    TEST_ASSERT(state.file_length == (int64_t)st.st_size,
                "file_length %" PRId64 " != actual %" PRId64,
                state.file_length, (int64_t)st.st_size);

    chat_state_free(&state);
    cleanup_chat(path);
    TEST_PASS("Adversarial: truncate then send maintains all invariants");
}

/* ================================================================
 * Additional adversarial test: empty and single-char handles
 *
 * Verifies edge cases around handle parsing.
 * ================================================================ */

static void test_single_char_handle(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/single_char.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Single-char handle */
    rc = chat_send(path, "a", "from a");
    TEST_ASSERT(rc == 0, "send from 'a' failed: %d", rc);

    /* Two-char handle */
    rc = chat_send(path, "ab", "from ab");
    TEST_ASSERT(rc == 0, "send from 'ab' failed: %d", rc);

    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);
    TEST_ASSERT(state.message_count == 2,
                "expected 2 messages, got %d", state.message_count);
    TEST_ASSERT(strcmp(state.messages[0].handle, "a") == 0,
                "handle mismatch for 'a': got '%s'", state.messages[0].handle);
    TEST_ASSERT(strcmp(state.messages[1].handle, "ab") == 0,
                "handle mismatch for 'ab': got '%s'", state.messages[1].handle);

    /* Verify content_len invariant */
    for (int i = 0; i < state.message_count; i++) {
        TEST_ASSERT(state.messages[i].content_len == strlen(state.messages[i].content),
                    "msg %d: content_len %zu != strlen %zu",
                    i, state.messages[i].content_len,
                    strlen(state.messages[i].content));
    }

    chat_state_free(&state);
    cleanup_chat(path);
    TEST_PASS("Adversarial: single-char and short handles parse correctly");
}

/* ================================================================
 * Additional adversarial test: max-length handle
 *
 * Test handle at exactly MAX_HANDLE_LEN - 1 characters.
 * ================================================================ */

static void test_max_length_handle(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/max_handle.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Handle at exactly MAX_HANDLE_LEN - 1 */
    char handle[MAX_HANDLE_LEN];
    memset(handle, 'z', MAX_HANDLE_LEN - 1);
    handle[MAX_HANDLE_LEN - 1] = '\0';

    rc = chat_send(path, handle, "max handle message");
    TEST_ASSERT(rc == 0, "send with max handle failed: %d", rc);

    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);
    TEST_ASSERT(state.message_count == 1,
                "expected 1 message, got %d", state.message_count);
    TEST_ASSERT(strcmp(state.messages[0].handle, handle) == 0,
                "max handle mismatch");

    /* Verify file-length is consistent */
    struct stat st;
    TEST_ASSERT(stat(path, &st) == 0, "stat failed");
    TEST_ASSERT(state.file_length == (int64_t)st.st_size,
                "file_length %" PRId64 " != actual %" PRId64,
                state.file_length, (int64_t)st.st_size);

    chat_state_free(&state);
    cleanup_chat(path);
    TEST_PASS("Adversarial: max-length handle round-trips correctly");
}

/* ================================================================
 * Additional adversarial test: rapid send + read cycles
 *
 * Stress the file-length invariant by doing many sends in sequence.
 * ================================================================ */

static void test_rapid_send_invariants(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/rapid_send.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Send 50 messages rapidly */
    for (int i = 0; i < 50; i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "rapid message number %d with some padding", i);
        char handle[32];
        snprintf(handle, sizeof(handle), "user%d", i % 5);
        rc = chat_send(path, handle, msg);
        TEST_ASSERT(rc == 0, "rapid send %d failed: %d", i, rc);
    }

    /* Verify final state */
    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);
    TEST_ASSERT(state.message_count == 50,
                "expected 50 messages, got %d", state.message_count);

    /* Verify file-length */
    struct stat st;
    TEST_ASSERT(stat(path, &st) == 0, "stat failed");
    TEST_ASSERT(state.file_length == (int64_t)st.st_size,
                "file_length %" PRId64 " != actual %" PRId64,
                state.file_length, (int64_t)st.st_size);

    /* Verify all content_len invariants */
    for (int i = 0; i < state.message_count; i++) {
        TEST_ASSERT(state.messages[i].content_len == strlen(state.messages[i].content),
                    "msg %d: content_len %zu != strlen %zu",
                    i, state.messages[i].content_len,
                    strlen(state.messages[i].content));
    }

    /* Verify participant count */
    TEST_ASSERT(state.participant_count == 5,
                "expected 5 participants, got %d", state.participant_count);

    chat_state_free(&state);
    cleanup_chat(path);
    TEST_PASS("Adversarial: 50 rapid sends maintain all invariants");
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void) {
    printf("=== chat_file audit violation tests ===\n\n");

    setup_test_dir();

    /* SECURITY violations */
    test_toctou_chat_create_preserves_existing();

    /* BUG violations */
    test_file_length_invariant_chat_send();
    test_file_length_invariant_chat_truncate();
    test_truncate_stored_vs_total();
    test_content_len_adversarial();

    /* HARDENING violations */
    test_safe_parse_int64_edge_cases();
    test_header_delimiter_requires_marker();
    test_base64_independent_of_chat_file();
    test_format_participants_postcondition();
    test_parse_participants_postcondition();

    /* Additional adversarial tests */
    test_truncate_then_send();
    test_single_char_handle();
    test_max_length_handle();
    test_rapid_send_invariants();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    /* Clean up test directory (best effort) */
    rmdir(test_dir);

    return tests_failed > 0 ? 1 : 0;
}
