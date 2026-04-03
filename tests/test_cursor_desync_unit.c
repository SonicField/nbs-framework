/*
 * test_cursor_desync_unit.c — Unit tests for cursor desync mitigations
 *
 * TDD red-phase tests. These test the behaviour specified in
 * feature-requests/cursor-desync-mitigations.md. Tests for features
 * that don't exist yet will FAIL until implementation.
 *
 * Falsifiable claims tested:
 *   1.  chat_cursor_read returns -1 for missing cursor file
 *   2.  chat_cursor_read returns -1 for missing handle
 *   3.  chat_cursor_write creates cursor file if missing
 *   4.  Cursor write preserves other handles' values
 *   5.  chat_cursor_write with msg_count-1 sets correct value
 *   6.  Many rapid sequential writes: final value correct
 *   7.  Concurrent cursor writes (different handles): all survive
 *   8.  Concurrent cursor writes (same handle): no corruption
 *   9.  Concurrent read+write: reads never see corrupt values
 *  10.  Fork stress: 20 children writing same cursor file
 *
 * Build (from src/nbs-chat/ via Makefile):
 *   make ../../tests/test_cursor_desync_unit
 *
 * Or manually:
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -I../src/nbs-chat \
 *       -o test_cursor_desync_unit test_cursor_desync_unit.c \
 *       ../src/nbs-chat/chat_file.c ../src/nbs-chat/lock.c \
 *       ../src/nbs-chat/base64.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#include "chat_file.h"
#include "lock.h"

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

/* --- Helpers --- */

static char test_dir[256];

static void setup_test_dir(void) {
    snprintf(test_dir, sizeof(test_dir), "/tmp/nbs_desync_test_XXXXXX");
    char *result = mkdtemp(test_dir);
    ASSERT_MSG(result != NULL, "mkdtemp failed: %s", strerror(errno));
}

static void rmrf(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

/* Create a minimal chat file with the given number of base64 message lines.
 * Messages are simple "sender: messageN" encoded in base64. */
static void create_test_chat(const char *path, int msg_count) {
    FILE *f = fopen(path, "w");
    ASSERT_MSG(f != NULL, "create_test_chat: fopen failed: %s", strerror(errno));

    fprintf(f,
        "=== nbs-chat ===\n"
        "last-writer: system\n"
        "last-write: 2026-04-02T00:00:00+0000\n"
        "file-length: 0\n"
        "participants: \n"
        "---\n");

    /* Write msg_count base64-encoded messages.
     * "agent: message N" → base64. We'll use a simple inline encoder
     * for test data since we just need valid non-empty lines. */
    for (int i = 0; i < msg_count; i++) {
        char raw[128];
        snprintf(raw, sizeof(raw), "agent|%ld: message %d",
                 (long)time(NULL), i + 1);

        /* Inline base64 encode (crude but sufficient for test data) */
        static const char b64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t raw_len = strlen(raw);
        size_t out_len = 4 * ((raw_len + 2) / 3);
        char *encoded = malloc(out_len + 1);
        ASSERT_MSG(encoded != NULL, "malloc failed");

        size_t j, k = 0;
        for (j = 0; j + 2 < raw_len; j += 3) {
            unsigned int n = ((unsigned char)raw[j] << 16) |
                             ((unsigned char)raw[j+1] << 8) |
                             (unsigned char)raw[j+2];
            encoded[k++] = b64[(n >> 18) & 0x3F];
            encoded[k++] = b64[(n >> 12) & 0x3F];
            encoded[k++] = b64[(n >> 6) & 0x3F];
            encoded[k++] = b64[n & 0x3F];
        }
        if (j < raw_len) {
            unsigned int n = (unsigned char)raw[j] << 16;
            if (j + 1 < raw_len) n |= (unsigned char)raw[j+1] << 8;
            encoded[k++] = b64[(n >> 18) & 0x3F];
            encoded[k++] = b64[(n >> 12) & 0x3F];
            encoded[k++] = (j + 1 < raw_len) ? b64[(n >> 6) & 0x3F] : '=';
            encoded[k++] = '=';
        }
        encoded[k] = '\0';

        fprintf(f, "%s\n", encoded);
        free(encoded);
    }

    fclose(f);
}

/* Write a cursor file directly (for test setup). */
static void write_cursor_file(const char *chat_path, const char *content) {
    char cpath[MAX_PATH_LEN];
    snprintf(cpath, sizeof(cpath), "%s.cursors", chat_path);
    FILE *f = fopen(cpath, "w");
    ASSERT_MSG(f != NULL, "write_cursor_file: fopen failed: %s", strerror(errno));
    fputs(content, f);
    fclose(f);
}

/* ============================================================
 * 1. Concurrent cursor writes (different handles): all survive
 *
 * Fork N children, each writing a unique handle. After all
 * complete, every handle must be present in the cursor file.
 * This exercises the lock under real concurrency.
 * ============================================================ */

static void test_concurrent_writes_different_handles(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/concurrent_diff.chat", test_dir);
    create_test_chat(path, 10);

    int N = 8;
    pid_t pids[8];

    for (int i = 0; i < N; i++) {
        pids[i] = fork();
        ASSERT_MSG(pids[i] >= 0, "fork failed: %s", strerror(errno));

        if (pids[i] == 0) {
            /* Child: write a unique handle */
            char handle[32];
            snprintf(handle, sizeof(handle), "worker%d", i);
            int rc = chat_cursor_write(path, handle, i + 1);
            _exit(rc == 0 ? 0 : 1);
        }
    }

    /* Wait for all children */
    int any_failed = 0;
    for (int i = 0; i < N; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            any_failed = 1;
        }
    }
    TEST_ASSERT(!any_failed, "one or more children failed");

    /* Verify all handles are present with correct values */
    for (int i = 0; i < N; i++) {
        char handle[32];
        snprintf(handle, sizeof(handle), "worker%d", i);
        int cursor = chat_cursor_read(path, handle);
        TEST_ASSERT(cursor == i + 1,
                    "handle '%s' expected cursor=%d, got %d",
                    handle, i + 1, cursor);
    }

    TEST_PASS("concurrent writes (different handles): all 8 survive with correct values");
}

/* ============================================================
 * 2. Concurrent cursor writes (same handle): no corruption
 *
 * Fork N children, all writing the same handle with different
 * values. The final value must be one of the written values
 * (no partial writes, no corruption).
 * ============================================================ */

static void test_concurrent_writes_same_handle(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/concurrent_same.chat", test_dir);
    create_test_chat(path, 100);

    int N = 10;
    pid_t pids[10];

    for (int i = 0; i < N; i++) {
        pids[i] = fork();
        ASSERT_MSG(pids[i] >= 0, "fork failed: %s", strerror(errno));

        if (pids[i] == 0) {
            int rc = chat_cursor_write(path, "shared", (i + 1) * 10);
            _exit(rc == 0 ? 0 : 1);
        }
    }

    int any_failed = 0;
    for (int i = 0; i < N; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            any_failed = 1;
        }
    }
    TEST_ASSERT(!any_failed, "one or more children failed");

    /* Final value must be one of 10, 20, 30, ..., 100 */
    int cursor = chat_cursor_read(path, "shared");
    int valid = 0;
    for (int i = 0; i < N; i++) {
        if (cursor == (i + 1) * 10) {
            valid = 1;
            break;
        }
    }
    TEST_ASSERT(valid,
                "final cursor=%d is not one of the written values (10,20,...,100)",
                cursor);

    TEST_PASS("concurrent writes (same handle): final value is valid (no corruption)");
}

/* ============================================================
 * 3. Cursor write preserves other handles' values
 *
 * Write handle A, then write handle B. Handle A must still
 * have its original value.
 * ============================================================ */

static void test_write_preserves_other_handles(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/preserve.chat", test_dir);
    create_test_chat(path, 10);

    int rc1 = chat_cursor_write(path, "alice", 5);
    TEST_ASSERT(rc1 == 0, "cursor_write for alice failed: %d", rc1);

    int rc2 = chat_cursor_write(path, "bob", 8);
    TEST_ASSERT(rc2 == 0, "cursor_write for bob failed: %d", rc2);

    /* Alice's cursor must still be 5 */
    int alice = chat_cursor_read(path, "alice");
    TEST_ASSERT(alice == 5,
                "alice cursor should be 5 after bob's write, got %d", alice);

    int bob = chat_cursor_read(path, "bob");
    TEST_ASSERT(bob == 8, "bob cursor should be 8, got %d", bob);

    TEST_PASS("write preserves other handles' values");
}

/* ============================================================
 * 6. chat_cursor_write with msg_count-1 sets correct value
 *
 * The fixup mitigation resets cursor to msg_count - 1 (0-indexed).
 * Verify that writing msg_count-1 works and reading it back
 * returns the same value.
 * ============================================================ */

static void test_cursor_write_msg_count_minus_one(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/msgcount.chat", test_dir);
    create_test_chat(path, 5);

    /* msg_count - 1 = 4 (0-indexed: messages 0,1,2,3,4) */
    int rc = chat_cursor_write(path, "agent", 4);
    TEST_ASSERT(rc == 0, "cursor_write(4) failed: %d", rc);

    int cursor = chat_cursor_read(path, "agent");
    TEST_ASSERT(cursor == 4, "cursor should be 4, got %d", cursor);

    TEST_PASS("cursor_write(msg_count-1) round-trips correctly");
}

/* ============================================================
 * 7. chat_cursor_read returns -1 for missing cursor file
 * ============================================================ */

static void test_cursor_read_missing_file(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/no_cursor.chat", test_dir);
    create_test_chat(path, 3);
    /* No cursor file created */

    int cursor = chat_cursor_read(path, "agent");
    TEST_ASSERT(cursor == -1,
                "cursor_read should return -1 for missing file, got %d", cursor);

    TEST_PASS("cursor_read returns -1 for missing cursor file");
}

/* ============================================================
 * 8. chat_cursor_read returns -1 for missing handle
 * ============================================================ */

static void test_cursor_read_missing_handle(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/missing_handle.chat", test_dir);
    create_test_chat(path, 3);

    write_cursor_file(path,
        "# Read cursors\n"
        "other=10\n");

    int cursor = chat_cursor_read(path, "agent");
    TEST_ASSERT(cursor == -1,
                "cursor_read should return -1 for missing handle, got %d", cursor);

    TEST_PASS("cursor_read returns -1 for missing handle");
}

/* ============================================================
 * 9. Concurrent read+write: reads never see corrupt values
 *
 * Fork a writer and a reader. The reader loops reading the
 * cursor. The writer loops writing incrementing values.
 * The reader should only see -1 (not yet written) or a
 * non-negative integer — never a corrupt partial value.
 * ============================================================ */

static void test_concurrent_read_write(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/readwrite.chat", test_dir);
    create_test_chat(path, 100);

    pid_t writer = fork();
    ASSERT_MSG(writer >= 0, "fork failed: %s", strerror(errno));

    if (writer == 0) {
        /* Writer child: write incrementing values */
        for (int i = 0; i < 50; i++) {
            chat_cursor_write(path, "target", i);
            usleep(1000); /* 1ms between writes */
        }
        _exit(0);
    }

    /* Parent: read in a loop, verify no corrupt values */
    int corrupt = 0;
    int reads = 0;
    for (int i = 0; i < 200; i++) {
        int cursor = chat_cursor_read(path, "target");
        reads++;
        if (cursor != -1 && (cursor < 0 || cursor >= 50)) {
            corrupt = 1;
            fprintf(stderr, "corrupt read: cursor=%d (expected -1 or [0,49])\n",
                    cursor);
            break;
        }
        usleep(500); /* 0.5ms between reads */
    }

    int status;
    waitpid(writer, &status, 0);
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "writer child failed");
    TEST_ASSERT(!corrupt,
                "reader saw corrupt value after %d reads", reads);

    TEST_PASS("concurrent read+write: no corrupt values seen");
}

/* ============================================================
 * 10. chat_cursor_write creates cursor file if missing
 * ============================================================ */

static void test_cursor_write_creates_file(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/autocreate.chat", test_dir);
    create_test_chat(path, 3);
    /* No cursor file exists yet */

    int rc = chat_cursor_write(path, "agent", 2);
    TEST_ASSERT(rc == 0, "cursor_write failed: %d", rc);

    int cursor = chat_cursor_read(path, "agent");
    TEST_ASSERT(cursor == 2, "cursor should be 2, got %d", cursor);

    TEST_PASS("cursor_write creates cursor file if missing");
}

/* ============================================================
 * 11. Many rapid sequential writes: final value correct
 *
 * Write 100 values to the same handle rapidly. The final
 * value must be the last one written.
 * ============================================================ */

static void test_rapid_sequential_writes(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/rapid.chat", test_dir);
    create_test_chat(path, 200);

    for (int i = 0; i < 100; i++) {
        int rc = chat_cursor_write(path, "agent", i);
        TEST_ASSERT(rc == 0, "cursor_write(%d) failed: %d", i, rc);
    }

    int cursor = chat_cursor_read(path, "agent");
    TEST_ASSERT(cursor == 99,
                "after 100 writes, cursor should be 99, got %d", cursor);

    TEST_PASS("100 rapid sequential writes: final value is 99");
}

/* ============================================================
 * 12. Fork stress: 20 children writing same cursor file
 *
 * 20 children each write their own handle. All 20 must be
 * present after. This is a higher-stress version of test 1.
 * ============================================================ */

static void test_fork_stress_20(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/stress20.chat", test_dir);
    create_test_chat(path, 50);

    int N = 20;
    pid_t pids[20];

    for (int i = 0; i < N; i++) {
        pids[i] = fork();
        ASSERT_MSG(pids[i] >= 0, "fork failed: %s", strerror(errno));

        if (pids[i] == 0) {
            char handle[32];
            snprintf(handle, sizeof(handle), "s%02d", i);
            /* Each child writes multiple times to stress the lock */
            for (int j = 0; j < 5; j++) {
                chat_cursor_write(path, handle, i * 5 + j);
            }
            _exit(0);
        }
    }

    for (int i = 0; i < N; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "child %d failed", i);
    }

    /* All 20 handles must be present */
    int missing = 0;
    for (int i = 0; i < N; i++) {
        char handle[32];
        snprintf(handle, sizeof(handle), "s%02d", i);
        int cursor = chat_cursor_read(path, handle);
        if (cursor < 0) {
            fprintf(stderr, "missing handle '%s'\n", handle);
            missing++;
        }
        /* Final value should be i*5+4 (last write: j=4) */
        int expected = i * 5 + 4;
        if (cursor >= 0 && cursor != expected) {
            /* Under concurrency, the final value should be the last
             * write from that child: i*5+4. If it's different,
             * something corrupted it. */
            fprintf(stderr, "handle '%s': expected %d, got %d\n",
                    handle, expected, cursor);
        }
    }
    TEST_ASSERT(missing == 0,
                "%d of %d handles missing after stress test", missing, N);

    TEST_PASS("fork stress: 20 children, all handles present");
}

/* --- Main --- */

int main(void) {
    printf("=== cursor desync unit tests ===\n\n");

    setup_test_dir();

    /* Core cursor operations */
    printf("--- Core cursor operations ---\n");
    test_cursor_read_missing_file();
    test_cursor_read_missing_handle();
    test_cursor_write_creates_file();
    test_write_preserves_other_handles();
    test_cursor_write_msg_count_minus_one();
    test_rapid_sequential_writes();

    /* Concurrent safety */
    printf("\n--- Concurrent safety ---\n");
    test_concurrent_writes_different_handles();
    test_concurrent_writes_same_handle();
    test_concurrent_read_write();
    test_fork_stress_20();

    /* Cleanup */
    rmrf(test_dir);

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
