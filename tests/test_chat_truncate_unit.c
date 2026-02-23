/*
 * test_chat_truncate_unit.c — Unit tests for chat_truncate.
 *
 * Falsifiable claims tested:
 *   1. Basic truncation: 5 messages → keep 3 → read back 3
 *   2. File-length header matches actual file size after truncate
 *   3. Last-writer matches the last kept message's handle
 *   4. Truncate to 0: header only, 0 messages
 *   5. Truncate to all: no-op, file unchanged
 *   6. Truncate beyond count: no-op
 *   7. File-length invariant holds under locking
 *   8. Integration: chat_read after truncate returns correct count
 */

#include "../src/nbs-chat/chat_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int tests = 0, fails = 0;

#define CHECK(label, cond) do { \
    tests++; \
    if (!(cond)) { \
        fails++; \
        printf("   FAIL: %s\n", label); \
    } else { \
        printf("   PASS: %s\n", label); \
    } \
} while(0)

static const char *TEST_PATH = "/tmp/test_chat_truncate_unit.chat";

/* Helper: create chat and send N messages from alternating handles */
static void setup_chat(int n) {
    unlink(TEST_PATH);
    chat_create(TEST_PATH);
    char msg[64];
    for (int i = 0; i < n; i++) {
        const char *handle = (i % 2 == 0) ? "alice" : "bob";
        snprintf(msg, sizeof(msg), "message %d", i);
        chat_send(TEST_PATH, handle, msg);
    }
}

/* Helper: get file size */
static int64_t file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

int main(void) {
    printf("test_chat_truncate_unit\n");

    /* 1. Basic truncation: 5 → 3 */
    {
        setup_chat(5);
        int rc = chat_truncate(TEST_PATH, 3);
        CHECK("basic truncate returns 0", rc == 0);
        chat_state_t state;
        rc = chat_read(TEST_PATH, &state);
        CHECK("basic truncate: read succeeds", rc == 0);
        CHECK("basic truncate: 3 messages remain", state.message_count == 3);
        /* Verify content of kept messages */
        CHECK("basic truncate: msg 0 is 'message 0'",
              strcmp(state.messages[0].content, "message 0") == 0);
        CHECK("basic truncate: msg 2 is 'message 2'",
              strcmp(state.messages[2].content, "message 2") == 0);
        chat_state_free(&state);
    }

    /* 2. File-length header matches actual file size */
    {
        setup_chat(5);
        chat_truncate(TEST_PATH, 3);
        chat_state_t state;
        chat_read(TEST_PATH, &state);
        int64_t actual = file_size(TEST_PATH);
        CHECK("file-length matches actual size",
              state.file_length == actual);
        chat_state_free(&state);
    }

    /* 3. Last-writer matches last kept message's handle */
    {
        setup_chat(5);
        /* Messages: alice(0), bob(1), alice(2), bob(3), alice(4) */
        /* Keep 3 → last is alice(2) */
        chat_truncate(TEST_PATH, 3);
        chat_state_t state;
        chat_read(TEST_PATH, &state);
        CHECK("last-writer is alice after truncate to 3",
              strcmp(state.last_writer, "alice") == 0);
        chat_state_free(&state);
    }

    /* 4. Truncate to 0 */
    {
        setup_chat(3);
        int rc = chat_truncate(TEST_PATH, 0);
        CHECK("truncate to 0 returns 0", rc == 0);
        chat_state_t state;
        rc = chat_read(TEST_PATH, &state);
        CHECK("truncate to 0: read succeeds", rc == 0);
        CHECK("truncate to 0: 0 messages", state.message_count == 0);
        int64_t actual = file_size(TEST_PATH);
        CHECK("truncate to 0: file-length matches",
              state.file_length == actual);
        chat_state_free(&state);
    }

    /* 5. Truncate to all: no-op */
    {
        setup_chat(3);
        /* Get size before */
        int64_t before = file_size(TEST_PATH);
        int rc = chat_truncate(TEST_PATH, 3);
        CHECK("truncate to all returns 0", rc == 0);
        int64_t after = file_size(TEST_PATH);
        CHECK("truncate to all: file unchanged", before == after);
    }

    /* 6. Truncate beyond count: no-op */
    {
        setup_chat(3);
        int64_t before = file_size(TEST_PATH);
        int rc = chat_truncate(TEST_PATH, 100);
        CHECK("truncate beyond count returns 0", rc == 0);
        int64_t after = file_size(TEST_PATH);
        CHECK("truncate beyond count: file unchanged", before == after);
    }

    /* 7. File-length invariant after truncate to 1 */
    {
        setup_chat(10);
        chat_truncate(TEST_PATH, 1);
        chat_state_t state;
        chat_read(TEST_PATH, &state);
        int64_t actual = file_size(TEST_PATH);
        CHECK("truncate to 1: file-length matches",
              state.file_length == actual);
        CHECK("truncate to 1: 1 message", state.message_count == 1);
        chat_state_free(&state);
    }

    /* 8. Integration: participants recomputed correctly */
    {
        setup_chat(5);
        /* Before truncate: alice(3), bob(2) */
        /* After truncate to 2: alice(1), bob(1) */
        chat_truncate(TEST_PATH, 2);
        chat_state_t state;
        chat_read(TEST_PATH, &state);
        CHECK("integration: 2 participants",
              state.participant_count == 2);
        /* Find alice and bob */
        int alice_count = -1, bob_count = -1;
        for (int i = 0; i < state.participant_count; i++) {
            if (strcmp(state.participants[i].handle, "alice") == 0)
                alice_count = state.participants[i].count;
            if (strcmp(state.participants[i].handle, "bob") == 0)
                bob_count = state.participants[i].count;
        }
        CHECK("integration: alice has 1 message", alice_count == 1);
        CHECK("integration: bob has 1 message", bob_count == 1);
        chat_state_free(&state);
    }

    /* Cleanup */
    unlink(TEST_PATH);
    char lock_path[256];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", TEST_PATH);
    unlink(lock_path);
    char cursor_path[256];
    snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", TEST_PATH);
    unlink(cursor_path);

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
