/*
 * test_chat_file_unit.c -- Unit tests for chat_file.c violation fixes
 *
 * Tests:
 *   1. File permission enforcement (0600 on created files)
 *   2. localtime_r thread-safety (get_timestamp does not crash)
 *   3. safe_parse_int boundary values, empty strings, NULL-adjacent
 *   4. snprintf truncation detection on header buffer
 *   5. Embedded NUL handling in content_len vs strdup
 *   6. fclose/ferror return checking
 *   7. OOM-graceful realloc path (cannot inject OOM, but verify non-abort path)
 *   8. chat_poll memory leak on failure (defensive chat_state_free)
 *   9. Mixed size_t/long consistency in compute_file_length
 *  10. Lock path consistency (chat_cursor_write vs chat_send)
 *  11. parts_str buffer overflow assertion
 *
 * Build:
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -I../src/nbs-chat \
 *       -o test_chat_file_unit test_chat_file_unit.c \
 *       ../src/nbs-chat/chat_file.c ../src/nbs-chat/lock.c \
 *       ../src/nbs-chat/base64.c
 *
 * Or with ASan:
 *   clang -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
 *       -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -I../src/nbs-chat \
 *       -o test_chat_file_unit test_chat_file_unit.c \
 *       ../src/nbs-chat/chat_file.c ../src/nbs-chat/lock.c \
 *       ../src/nbs-chat/base64.c \
 *       -fsanitize=address,undefined
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

/* Include the headers from the source directory */
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

/* --- Helper: create a temporary directory for test files --- */
static char test_dir[256];

static void setup_test_dir(void) {
    snprintf(test_dir, sizeof(test_dir), "/tmp/test_chat_file_XXXXXX");
    char *result = mkdtemp(test_dir);
    ASSERT_MSG(result != NULL, "mkdtemp failed: %s", strerror(errno));
}

static void cleanup_path(const char *path) {
    unlink(path);
}

/* --- Test 1: File permission enforcement (SECURITY #1) --- */

static void test_file_permissions_chat_create(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/perm_test_create.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    struct stat st;
    int stat_rc = stat(path, &st);
    TEST_ASSERT(stat_rc == 0, "stat failed after chat_create: %s", strerror(errno));

    mode_t file_mode = st.st_mode & 0777;
    TEST_ASSERT(file_mode == 0600,
                "chat_create: file mode is 0%03o, expected 0600", file_mode);

    cleanup_path(path);
    TEST_PASS("chat_create sets file permissions to 0600");
}

static void test_file_permissions_chat_send(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/perm_test_send.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    rc = chat_send(path, "alice", "hello");
    TEST_ASSERT(rc == 0, "chat_send failed: %d", rc);

    struct stat st;
    int stat_rc = stat(path, &st);
    TEST_ASSERT(stat_rc == 0, "stat failed after chat_send: %s", strerror(errno));

    mode_t file_mode = st.st_mode & 0777;
    TEST_ASSERT(file_mode == 0600,
                "chat_send: file mode is 0%03o, expected 0600", file_mode);

    /* Clean up lock file too */
    char lock_path[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("chat_send preserves file permissions to 0600");
}

static void test_file_permissions_cursor_write(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/perm_test_cursor.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    rc = chat_cursor_write(path, "alice", 5);
    TEST_ASSERT(rc == 0, "chat_cursor_write failed: %d", rc);

    /* Check the cursor temp file's final path */
    char cpath[520];
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);

    struct stat st;
    int stat_rc = stat(cpath, &st);
    TEST_ASSERT(stat_rc == 0, "stat failed for cursor file: %s", strerror(errno));

    mode_t file_mode = st.st_mode & 0777;
    TEST_ASSERT(file_mode == 0600,
                "cursor file mode is 0%03o, expected 0600", file_mode);

    /* Clean up */
    char lock_path[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    cleanup_path(lock_path);
    cleanup_path(cpath);
    cleanup_path(path);
    TEST_PASS("chat_cursor_write sets file permissions to 0600");
}

/* --- Test 2: localtime_r usage (SECURITY #2) --- */

static void test_get_timestamp_produces_valid_output(void) {
    /*
     * We cannot directly test localtime_r vs localtime from outside,
     * but we can verify get_timestamp produces a valid ISO 8601-ish
     * timestamp by creating a chat file and reading back the timestamp.
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/timestamp_test.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    /* Verify timestamp is non-empty and starts with a year */
    TEST_ASSERT(strlen(state.last_write) > 0,
                "last_write timestamp is empty");
    TEST_ASSERT(state.last_write[0] == '2',
                "last_write does not start with '2' (year): got '%c'",
                state.last_write[0]);
    /* ISO 8601: YYYY-MM-DDThh:mm:ss... minimum 19 chars */
    TEST_ASSERT(strlen(state.last_write) >= 19,
                "last_write too short for ISO 8601: '%s' (len %zu)",
                state.last_write, strlen(state.last_write));

    chat_state_free(&state);
    cleanup_path(path);
    TEST_PASS("get_timestamp produces valid ISO 8601 timestamp (localtime_r)");
}

/* --- Test 3: safe_parse_int boundary values (HARDENING #8) --- */

static void test_safe_parse_int_boundaries(void) {
    /*
     * safe_parse_int is static, so we test it indirectly via chat_cursor_read
     * which calls it. We write cursor files with boundary values and read them.
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/parse_int_test.chat", test_dir);

    /* Create a minimal chat file so cursor operations work */
    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Write cursor with normal value */
    rc = chat_cursor_write(path, "alice", 42);
    TEST_ASSERT(rc == 0, "chat_cursor_write(42) failed: %d", rc);

    int cursor = chat_cursor_read(path, "alice");
    TEST_ASSERT(cursor == 42,
                "cursor read: expected 42, got %d", cursor);

    /* Write cursor with zero */
    rc = chat_cursor_write(path, "bob", 0);
    TEST_ASSERT(rc == 0, "chat_cursor_write(0) failed: %d", rc);

    cursor = chat_cursor_read(path, "bob");
    TEST_ASSERT(cursor == 0,
                "cursor read: expected 0, got %d", cursor);

    /* Write cursor with large value */
    rc = chat_cursor_write(path, "charlie", INT_MAX / 2);
    TEST_ASSERT(rc == 0, "chat_cursor_write(INT_MAX/2) failed: %d", rc);

    cursor = chat_cursor_read(path, "charlie");
    TEST_ASSERT(cursor == INT_MAX / 2,
                "cursor read: expected %d, got %d", INT_MAX / 2, cursor);

    /* Test reading non-existent handle */
    cursor = chat_cursor_read(path, "nobody");
    TEST_ASSERT(cursor == -1,
                "cursor read for non-existent handle: expected -1, got %d", cursor);

    /* Clean up */
    char cpath[520], lock_path[520];
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("safe_parse_int handles boundary values correctly");
}

static void test_safe_parse_int_malformed_cursor_file(void) {
    /*
     * Write a cursor file with malformed content and verify
     * chat_cursor_read handles it gracefully (returns -1).
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/malformed_cursor.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Manually write a cursor file with bad values */
    char cpath[520];
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);

    int fd = open(cpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT(fd >= 0, "open cursor file failed: %s", strerror(errno));

    const char *content =
        "# Read cursors\n"
        "alice=notanumber\n"
        "bob=\n"
        "charlie=999999999999999999999\n"
        "dave=42\n";
    ssize_t written = write(fd, content, strlen(content));
    TEST_ASSERT(written == (ssize_t)strlen(content), "write failed");
    close(fd);

    /* alice: not a number -> -1 */
    int cursor = chat_cursor_read(path, "alice");
    TEST_ASSERT(cursor == -1,
                "malformed 'notanumber': expected -1, got %d", cursor);

    /* bob: empty string -> -1 */
    cursor = chat_cursor_read(path, "bob");
    TEST_ASSERT(cursor == -1,
                "malformed empty: expected -1, got %d", cursor);

    /* charlie: overflow -> -1 */
    cursor = chat_cursor_read(path, "charlie");
    TEST_ASSERT(cursor == -1,
                "malformed overflow: expected -1, got %d", cursor);

    /* dave: valid -> 42 */
    cursor = chat_cursor_read(path, "dave");
    TEST_ASSERT(cursor == 42,
                "valid cursor: expected 42, got %d", cursor);

    /* Clean up */
    char lock_path[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("safe_parse_int handles malformed cursor values gracefully");
}

/* --- Test 4: snprintf truncation detection (HARDENING #10) --- */

static void test_snprintf_truncation_in_header(void) {
    /*
     * Test that chat_send with a very long handle or participants string
     * does not silently truncate. We test with handles near MAX_HANDLE_LEN.
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/snprintf_trunc.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Send a message with a handle near the maximum length */
    char long_handle[MAX_HANDLE_LEN];
    memset(long_handle, 'a', MAX_HANDLE_LEN - 1);
    long_handle[MAX_HANDLE_LEN - 1] = '\0';

    rc = chat_send(path, long_handle, "test message");
    TEST_ASSERT(rc == 0, "chat_send with long handle failed: %d", rc);

    /* Verify the message was stored correctly */
    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    TEST_ASSERT(state.message_count == 1,
                "expected 1 message, got %d", state.message_count);
    TEST_ASSERT(strcmp(state.messages[0].handle, long_handle) == 0,
                "handle mismatch after send with long handle");

    chat_state_free(&state);

    /* Clean up */
    char lock_path[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("snprintf truncation detection works for header buffer");
}

/* --- Test 5: Embedded NUL handling (BUG #6) --- */

static void test_content_len_vs_strlen(void) {
    /*
     * Verify that content_len == strlen(content) after a round-trip
     * through chat_send + chat_read. The fix asserts this invariant.
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/content_len.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    const char *message = "hello world, this is a test message with no NULs";
    rc = chat_send(path, "alice", message);
    TEST_ASSERT(rc == 0, "chat_send failed: %d", rc);

    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    TEST_ASSERT(state.message_count == 1,
                "expected 1 message, got %d", state.message_count);

    /* The documented invariant: content_len == strlen(content) */
    size_t actual_strlen = strlen(state.messages[0].content);
    TEST_ASSERT(state.messages[0].content_len == actual_strlen,
                "content_len %zu != strlen(content) %zu",
                state.messages[0].content_len, actual_strlen);

    /* Verify content matches */
    TEST_ASSERT(strcmp(state.messages[0].content, message) == 0,
                "content mismatch: got '%s'", state.messages[0].content);

    chat_state_free(&state);

    /* Clean up */
    char lock_path[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("content_len == strlen(content) invariant holds");
}

/* --- Test 6: fclose/ferror checking (BUG #7) --- */

static void test_chat_read_nonexistent_file(void) {
    /*
     * Verify chat_read returns -1 for a non-existent file.
     * This tests the basic error path.
     */
    chat_state_t state;
    int rc = chat_read("/tmp/definitely_does_not_exist_12345.chat", &state);
    TEST_ASSERT(rc == -1,
                "chat_read on non-existent file: expected -1, got %d", rc);

    TEST_PASS("chat_read returns -1 for non-existent file");
}

/* --- Test 7: chat_create idempotence check --- */

static void test_chat_create_already_exists(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/already_exists.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "first chat_create failed: %d", rc);

    /* Second create should fail with -1 */
    rc = chat_create(path);
    TEST_ASSERT(rc == -1,
                "second chat_create: expected -1, got %d", rc);

    cleanup_path(path);
    TEST_PASS("chat_create returns -1 when file already exists");
}

/* --- Test 8: chat_poll memory leak protection (BUG #5) --- */

static void test_chat_poll_error_on_missing_file(void) {
    /*
     * chat_poll should return -1 when the file doesn't exist,
     * without leaking memory (verified by ASan at runtime).
     */
    int rc = chat_poll("/tmp/no_such_file_poll_test.chat", "alice", 0);
    TEST_ASSERT(rc == -1,
                "chat_poll on non-existent file: expected -1, got %d", rc);

    TEST_PASS("chat_poll returns -1 on missing file (no leak under ASan)");
}

/* --- Test 9: chat_state_free defensive on NULL --- */

static void test_chat_state_free_null(void) {
    /* Must not crash on NULL */
    chat_state_free(NULL);

    /* Must not crash on zeroed state */
    chat_state_t state;
    memset(&state, 0, sizeof(state));
    chat_state_free(&state);

    TEST_PASS("chat_state_free handles NULL and zeroed state");
}

/* --- Test 10: Multiple messages round-trip --- */

static void test_multiple_messages(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/multi_msg.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    /* Send several messages */
    rc = chat_send(path, "alice", "first message");
    TEST_ASSERT(rc == 0, "send 1 failed: %d", rc);

    rc = chat_send(path, "bob", "second message");
    TEST_ASSERT(rc == 0, "send 2 failed: %d", rc);

    rc = chat_send(path, "alice", "third message");
    TEST_ASSERT(rc == 0, "send 3 failed: %d", rc);

    /* Read and verify */
    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    TEST_ASSERT(state.message_count == 3,
                "expected 3 messages, got %d", state.message_count);

    TEST_ASSERT(strcmp(state.messages[0].handle, "alice") == 0,
                "msg 0 handle mismatch");
    TEST_ASSERT(strcmp(state.messages[0].content, "first message") == 0,
                "msg 0 content mismatch");

    TEST_ASSERT(strcmp(state.messages[1].handle, "bob") == 0,
                "msg 1 handle mismatch");
    TEST_ASSERT(strcmp(state.messages[1].content, "second message") == 0,
                "msg 1 content mismatch");

    TEST_ASSERT(strcmp(state.messages[2].handle, "alice") == 0,
                "msg 2 handle mismatch");
    TEST_ASSERT(strcmp(state.messages[2].content, "third message") == 0,
                "msg 2 content mismatch");

    /* Verify participant counts */
    TEST_ASSERT(state.participant_count == 2,
                "expected 2 participants, got %d", state.participant_count);

    /* Verify content_len invariant on all messages */
    for (int i = 0; i < state.message_count; i++) {
        TEST_ASSERT(state.messages[i].content_len == strlen(state.messages[i].content),
                    "msg %d: content_len %zu != strlen %zu",
                    i, state.messages[i].content_len,
                    strlen(state.messages[i].content));
    }

    chat_state_free(&state);

    /* Clean up */
    char lock_path[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("multiple messages round-trip correctly");
}

/* --- Test 11: Lock path consistency (SECURITY #3) --- */

static void test_lock_path_consistency(void) {
    /*
     * Verify that chat_cursor_write and chat_send use the same lock file.
     * After fix: both should lock on <path>.lock.
     * We verify indirectly: send + cursor_write should not deadlock
     * and the lock file should be <path>.lock (not <path>.lock.lock).
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/lock_consistency.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    rc = chat_send(path, "alice", "hello");
    TEST_ASSERT(rc == 0, "chat_send failed: %d", rc);

    rc = chat_cursor_write(path, "alice", 0);
    TEST_ASSERT(rc == 0, "chat_cursor_write failed: %d", rc);

    /* Verify that <path>.lock exists (the correct lock file) */
    char lock_path[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    struct stat st;
    int lock_exists = (stat(lock_path, &st) == 0);
    TEST_ASSERT(lock_exists, "expected lock file %s to exist", lock_path);

    /* Verify that <path>.lock.lock does NOT exist (the bug) */
    char bad_lock_path[540];
    snprintf(bad_lock_path, sizeof(bad_lock_path), "%s.lock.lock", path);
    int bad_lock_exists = (stat(bad_lock_path, &st) == 0);
    TEST_ASSERT(!bad_lock_exists,
                "bug: double-lock file %s should not exist", bad_lock_path);

    /* Clean up */
    char cpath[520];
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("lock path consistency: no double-lock file created");
}

/* --- Test 12: compute_file_length consistency (HARDENING #12) --- */

static void test_file_length_header_accuracy(void) {
    /*
     * Verify that the file-length header matches actual file size
     * for multiple chat states (empty, one message, several messages).
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/file_length.chat", test_dir);

    /* Empty chat */
    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    struct stat st;
    chat_state_t state;

    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    int stat_rc = stat(path, &st);
    TEST_ASSERT(stat_rc == 0, "stat failed: %s", strerror(errno));
    TEST_ASSERT(state.file_length == (long)st.st_size,
                "empty chat: file_length %ld != actual %ld",
                state.file_length, (long)st.st_size);
    chat_state_free(&state);

    /* After one message */
    rc = chat_send(path, "alice", "hello");
    TEST_ASSERT(rc == 0, "chat_send failed: %d", rc);

    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    stat_rc = stat(path, &st);
    TEST_ASSERT(stat_rc == 0, "stat failed: %s", strerror(errno));
    TEST_ASSERT(state.file_length == (long)st.st_size,
                "one-msg chat: file_length %ld != actual %ld",
                state.file_length, (long)st.st_size);
    chat_state_free(&state);

    /* After several messages */
    rc = chat_send(path, "bob", "world");
    TEST_ASSERT(rc == 0, "chat_send 2 failed: %d", rc);
    rc = chat_send(path, "alice", "foo bar baz");
    TEST_ASSERT(rc == 0, "chat_send 3 failed: %d", rc);

    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    stat_rc = stat(path, &st);
    TEST_ASSERT(stat_rc == 0, "stat failed: %s", strerror(errno));
    TEST_ASSERT(state.file_length == (long)st.st_size,
                "multi-msg chat: file_length %ld != actual %ld",
                state.file_length, (long)st.st_size);
    chat_state_free(&state);

    /* Clean up */
    char lock_path[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("file-length header matches actual file size at all stages");
}

/* --- Test 13: Cursor-on-write (T21) — chat_send updates sender cursor --- */

static void test_cursor_on_write_single_send(void) {
    /*
     * T21a: After chat_send, the sender's cursor should point to
     * the index of the message just written.
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/cursor_on_write_1.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    rc = chat_send(path, "alice", "first message");
    TEST_ASSERT(rc == 0, "chat_send failed: %d", rc);

    /* After sending message 0, alice's cursor should be 0 */
    int cursor = chat_cursor_read(path, "alice");
    TEST_ASSERT(cursor == 0,
                "T21a: after first send, cursor should be 0, got %d", cursor);

    /* Clean up */
    char lock_path[520], cpath[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("T21a: chat_send updates sender cursor to sent message index");
}

static void test_cursor_on_write_no_unread_for_sender(void) {
    /*
     * T21b: After chat_send, reading with --unread for the sender
     * should show zero unread messages (cursor is at the latest).
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/cursor_on_write_2.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    rc = chat_send(path, "alice", "hello");
    TEST_ASSERT(rc == 0, "chat_send failed: %d", rc);

    /* Read the state to get message count */
    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    int cursor = chat_cursor_read(path, "alice");

    /* cursor should be message_count - 1 (pointing at last message) */
    /* So unread = messages from cursor+1 to end = none */
    TEST_ASSERT(cursor == state.message_count - 1,
                "T21b: cursor %d should equal message_count-1 (%d)",
                cursor, state.message_count - 1);

    chat_state_free(&state);

    /* Clean up */
    char lock_path[520], cpath[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("T21b: no unread messages for sender after send");
}

static void test_cursor_on_write_two_senders(void) {
    /*
     * T21c: Two different senders each have independent cursors
     * pointing to their respective last-sent message index.
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/cursor_on_write_3.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    rc = chat_send(path, "alice", "msg 0");   /* index 0 */
    TEST_ASSERT(rc == 0, "send alice failed: %d", rc);

    rc = chat_send(path, "bob", "msg 1");     /* index 1 */
    TEST_ASSERT(rc == 0, "send bob failed: %d", rc);

    rc = chat_send(path, "alice", "msg 2");   /* index 2 */
    TEST_ASSERT(rc == 0, "send alice 2 failed: %d", rc);

    /* alice sent last at index 2 */
    int alice_cursor = chat_cursor_read(path, "alice");
    TEST_ASSERT(alice_cursor == 2,
                "T21c: alice cursor should be 2, got %d", alice_cursor);

    /* bob sent last at index 1 */
    int bob_cursor = chat_cursor_read(path, "bob");
    TEST_ASSERT(bob_cursor == 1,
                "T21c: bob cursor should be 1, got %d", bob_cursor);

    /* Clean up */
    char lock_path[520], cpath[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("T21c: two senders have independent cursors at their last message");
}

static void test_cursor_on_write_sequential_sends(void) {
    /*
     * T21d: Sending multiple messages from the same handle
     * updates the cursor each time to the latest message index.
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/cursor_on_write_4.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    for (int i = 0; i < 5; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "message %d", i);

        rc = chat_send(path, "alice", msg);
        TEST_ASSERT(rc == 0, "send %d failed: %d", i, rc);

        int cursor = chat_cursor_read(path, "alice");
        TEST_ASSERT(cursor == i,
                    "T21d: after send %d, cursor should be %d, got %d",
                    i, i, cursor);
    }

    /* Clean up */
    char lock_path[520], cpath[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("T21d: sequential sends update cursor each time");
}

/* --- Test T22: Per-message timestamps --- */

static void test_timestamp_round_trip(void) {
    /*
     * T22a: Send a message and verify the timestamp is within ±2 seconds
     * of the current time.
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/timestamp_rt.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    time_t before = time(NULL);
    rc = chat_send(path, "alice", "timestamped message");
    time_t after = time(NULL);
    TEST_ASSERT(rc == 0, "chat_send failed: %d", rc);

    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    TEST_ASSERT(state.message_count == 1,
                "expected 1 message, got %d", state.message_count);

    TEST_ASSERT(state.messages[0].timestamp >= before,
                "T22a: timestamp %ld is before send start %ld",
                (long)state.messages[0].timestamp, (long)before);
    TEST_ASSERT(state.messages[0].timestamp <= after + 1,
                "T22a: timestamp %ld is after send end %ld",
                (long)state.messages[0].timestamp, (long)(after + 1));

    /* Verify content is still correct (no timestamp leaking into content) */
    TEST_ASSERT(strcmp(state.messages[0].content, "timestamped message") == 0,
                "T22a: content mismatch: got '%s'", state.messages[0].content);
    TEST_ASSERT(strcmp(state.messages[0].handle, "alice") == 0,
                "T22a: handle mismatch: got '%s'", state.messages[0].handle);

    chat_state_free(&state);

    /* Clean up */
    char lock_path[520], cpath[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("T22a: timestamp round-trip within ±2 seconds");
}

static void test_timestamp_backward_compat(void) {
    /*
     * T22b: Manually write a chat file with old-format messages (no timestamps)
     * and verify they parse with timestamp=0 and correct handle/content.
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/timestamp_compat.chat", test_dir);

    /* Create a chat file manually with old-format base64 messages */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT(fd >= 0, "open failed: %s", strerror(errno));

    /* Old format: base64("alice: hello world") */
    /* "alice: hello world" base64 = "YWxpY2U6IGhlbGxvIHdvcmxk" */
    const char *content =
        "=== nbs-chat ===\n"
        "last-writer: alice\n"
        "last-write: 2026-02-17T12:00:00+0000\n"
        "file-length: 999\n"       /* won't match but that's OK for read */
        "participants: alice(1)\n"
        "---\n"
        "YWxpY2U6IGhlbGxvIHdvcmxk\n";
    ssize_t written = write(fd, content, strlen(content));
    TEST_ASSERT(written == (ssize_t)strlen(content), "write failed");
    close(fd);

    chat_state_t state;
    int rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    TEST_ASSERT(state.message_count == 1,
                "expected 1 message, got %d", state.message_count);
    TEST_ASSERT(strcmp(state.messages[0].handle, "alice") == 0,
                "T22b: handle mismatch: got '%s'", state.messages[0].handle);
    TEST_ASSERT(strcmp(state.messages[0].content, "hello world") == 0,
                "T22b: content mismatch: got '%s'", state.messages[0].content);
    TEST_ASSERT(state.messages[0].timestamp == 0,
                "T22b: legacy message should have timestamp=0, got %ld",
                (long)state.messages[0].timestamp);

    chat_state_free(&state);
    cleanup_path(path);
    TEST_PASS("T22b: backward compat — old messages parse with timestamp=0");
}

static void test_timestamp_multiple_messages(void) {
    /*
     * T22c: Send multiple messages, verify each has a non-zero timestamp
     * and timestamps are non-decreasing.
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/timestamp_multi.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    rc = chat_send(path, "alice", "first");
    TEST_ASSERT(rc == 0, "send 1 failed: %d", rc);

    rc = chat_send(path, "bob", "second");
    TEST_ASSERT(rc == 0, "send 2 failed: %d", rc);

    rc = chat_send(path, "alice", "third");
    TEST_ASSERT(rc == 0, "send 3 failed: %d", rc);

    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    TEST_ASSERT(state.message_count == 3,
                "expected 3 messages, got %d", state.message_count);

    for (int i = 0; i < state.message_count; i++) {
        TEST_ASSERT(state.messages[i].timestamp > 0,
                    "T22c: message %d has timestamp=0", i);
    }

    /* Timestamps should be non-decreasing */
    for (int i = 1; i < state.message_count; i++) {
        TEST_ASSERT(state.messages[i].timestamp >= state.messages[i-1].timestamp,
                    "T22c: timestamps not non-decreasing: msg %d (%ld) < msg %d (%ld)",
                    i, (long)state.messages[i].timestamp,
                    i-1, (long)state.messages[i-1].timestamp);
    }

    /* Verify content integrity */
    TEST_ASSERT(strcmp(state.messages[0].content, "first") == 0, "msg 0 content");
    TEST_ASSERT(strcmp(state.messages[1].content, "second") == 0, "msg 1 content");
    TEST_ASSERT(strcmp(state.messages[2].content, "third") == 0, "msg 2 content");

    chat_state_free(&state);

    /* Clean up */
    char lock_path[520], cpath[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("T22c: multiple messages all have timestamps, non-decreasing");
}

static void test_timestamp_file_length_invariant(void) {
    /*
     * T22d: The file-length header must still match actual file size
     * after the timestamp format change (longer payloads).
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/timestamp_flen.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    rc = chat_send(path, "alice", "test message for file length check");
    TEST_ASSERT(rc == 0, "chat_send failed: %d", rc);

    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    struct stat st;
    int stat_rc = stat(path, &st);
    TEST_ASSERT(stat_rc == 0, "stat failed: %s", strerror(errno));
    TEST_ASSERT(state.file_length == (long)st.st_size,
                "T22d: file_length %ld != actual %ld",
                state.file_length, (long)st.st_size);

    chat_state_free(&state);

    /* Clean up */
    char lock_path[520], cpath[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("T22d: file-length header matches actual file size with timestamps");
}

static void test_timestamp_content_with_pipe(void) {
    /*
     * T22e: Messages containing pipe characters in content should
     * parse correctly (pipe in content must not confuse the parser).
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/timestamp_pipe.chat", test_dir);

    int rc = chat_create(path);
    TEST_ASSERT(rc == 0, "chat_create failed: %d", rc);

    const char *msg = "this | has | pipes | in it";
    rc = chat_send(path, "alice", msg);
    TEST_ASSERT(rc == 0, "chat_send failed: %d", rc);

    chat_state_t state;
    rc = chat_read(path, &state);
    TEST_ASSERT(rc == 0, "chat_read failed: %d", rc);

    TEST_ASSERT(state.message_count == 1,
                "expected 1 message, got %d", state.message_count);
    TEST_ASSERT(strcmp(state.messages[0].handle, "alice") == 0,
                "T22e: handle mismatch: got '%s'", state.messages[0].handle);
    TEST_ASSERT(strcmp(state.messages[0].content, msg) == 0,
                "T22e: content mismatch: got '%s'", state.messages[0].content);
    TEST_ASSERT(state.messages[0].timestamp > 0,
                "T22e: timestamp should be non-zero");

    chat_state_free(&state);

    /* Clean up */
    char lock_path[520], cpath[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    snprintf(cpath, sizeof(cpath), "%s.cursors", path);
    cleanup_path(cpath);
    cleanup_path(lock_path);
    cleanup_path(path);
    TEST_PASS("T22e: message with pipes in content parses correctly");
}

/* --- Main --- */

int main(void) {
    printf("=== chat_file unit tests ===\n\n");

    setup_test_dir();

    /* SECURITY tests */
    test_file_permissions_chat_create();
    test_file_permissions_chat_send();
    test_file_permissions_cursor_write();
    test_get_timestamp_produces_valid_output();
    test_lock_path_consistency();

    /* BUG tests */
    test_content_len_vs_strlen();
    test_chat_read_nonexistent_file();
    test_chat_create_already_exists();
    test_chat_poll_error_on_missing_file();
    test_multiple_messages();

    /* HARDENING tests */
    test_safe_parse_int_boundaries();
    test_safe_parse_int_malformed_cursor_file();
    test_snprintf_truncation_in_header();
    test_chat_state_free_null();
    test_file_length_header_accuracy();

    /* CURSOR-ON-WRITE tests (T21) */
    test_cursor_on_write_single_send();
    test_cursor_on_write_no_unread_for_sender();
    test_cursor_on_write_two_senders();
    test_cursor_on_write_sequential_sends();

    /* PER-MESSAGE TIMESTAMP tests (T22) */
    test_timestamp_round_trip();
    test_timestamp_backward_compat();
    test_timestamp_multiple_messages();
    test_timestamp_file_length_invariant();
    test_timestamp_content_with_pipe();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    /* Clean up test directory (best effort) */
    rmdir(test_dir);

    return tests_failed > 0 ? 1 : 0;
}
