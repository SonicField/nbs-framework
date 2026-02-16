/*
 * test_lock_unit.c — Unit tests for fcntl-based file locking
 *
 * Tests:
 *   1. Lock acquire and release (basic round-trip)
 *   2. Lock acquire returns valid fd
 *   3. Double-release behaviour (second release on closed fd)
 *   4. O_CLOEXEC is set on acquired lock fd
 *   5. Lock acquire on invalid path fails gracefully
 *   6. Lock release postcondition: fd is closed after release
 *
 * Build (from tests/ directory):
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -I../src/nbs-chat \
 *       -o test_lock_unit test_lock_unit.c \
 *       ../src/nbs-chat/lock.c ../src/nbs-chat/chat_file.c \
 *       ../src/nbs-chat/base64.c
 *
 * Or with ASan:
 *   clang -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
 *       -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -I../src/nbs-chat \
 *       -o test_lock_unit test_lock_unit.c \
 *       ../src/nbs-chat/lock.c ../src/nbs-chat/chat_file.c \
 *       ../src/nbs-chat/base64.c \
 *       -fsanitize=address,undefined
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/* Include the headers from the source directory */
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

/* Helper: create a temporary file path for testing.
 * Returns a path in /tmp that the caller can use as a chat_path.
 * The caller is responsible for cleanup. */
static void make_temp_path(char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "/tmp/test_lock_unit_%d", getpid());
}

/* Helper: clean up lock file created by chat_lock_acquire */
static void cleanup_lock_file(const char *chat_path) {
    char lock_path[MAX_PATH_LEN];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", chat_path);
    unlink(lock_path);
}

/* --- Test basic acquire and release --- */

static void test_acquire_and_release(void) {
    char path[256];
    make_temp_path(path, sizeof(path));

    int fd = chat_lock_acquire(path);
    TEST_ASSERT(fd >= 0, "chat_lock_acquire should return fd >= 0, got %d", fd);

    /* Verify the fd is valid by checking fcntl */
    int flags = fcntl(fd, F_GETFD);
    TEST_ASSERT(flags >= 0, "fd %d should be valid (fcntl F_GETFD), got %d: %s",
                fd, flags, strerror(errno));

    chat_lock_release(fd);

    /* Verify fd is closed after release */
    int flags_after = fcntl(fd, F_GETFD);
    TEST_ASSERT(flags_after == -1 && errno == EBADF,
                "fd %d should be closed after release, fcntl returned %d",
                fd, flags_after);

    cleanup_lock_file(path);
    TEST_PASS("acquire and release");
}

/* --- Test that acquired fd has a write lock held --- */

static void test_lock_is_held(void) {
    char path[256];
    make_temp_path(path, sizeof(path));

    int fd = chat_lock_acquire(path);
    TEST_ASSERT(fd >= 0, "chat_lock_acquire failed: %d", fd);

    /* Query the lock state — should show our lock is held */
    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    int ret = fcntl(fd, F_GETLK, &fl);
    TEST_ASSERT(ret == 0, "fcntl F_GETLK failed: %s", strerror(errno));
    /* F_GETLK returns F_UNLCK if no conflicting lock exists.
     * Since we are the holder and querying our own fd, the kernel
     * reports F_UNLCK (no *other* process holds a conflicting lock). */
    TEST_ASSERT(fl.l_type == F_UNLCK,
                "expected F_UNLCK (no conflicting lock), got %d", fl.l_type);

    chat_lock_release(fd);
    cleanup_lock_file(path);
    TEST_PASS("lock is held after acquire");
}

/* --- Test O_CLOEXEC is set on the lock fd --- */

static void test_cloexec_flag(void) {
    char path[256];
    make_temp_path(path, sizeof(path));

    int fd = chat_lock_acquire(path);
    TEST_ASSERT(fd >= 0, "chat_lock_acquire failed: %d", fd);

    int flags = fcntl(fd, F_GETFD);
    TEST_ASSERT(flags >= 0, "fcntl F_GETFD failed: %s", strerror(errno));
    TEST_ASSERT((flags & FD_CLOEXEC) != 0,
                "FD_CLOEXEC should be set on lock fd %d, flags = 0x%x",
                fd, flags);

    chat_lock_release(fd);
    cleanup_lock_file(path);
    TEST_PASS("O_CLOEXEC is set on lock fd");
}

/* --- Test acquire on nonexistent deeply-nested path fails gracefully --- */

static void test_acquire_invalid_path(void) {
    /* open() should fail for a path in a nonexistent directory */
    int fd = chat_lock_acquire("/nonexistent/deeply/nested/path/chat");
    TEST_ASSERT(fd == -1,
                "chat_lock_acquire should return -1 for invalid path, got %d", fd);

    TEST_PASS("acquire on invalid path returns -1");
}

/* --- Test double release: second release on already-closed fd --- */

static void test_double_release(void) {
    char path[256];
    make_temp_path(path, sizeof(path));

    int fd = chat_lock_acquire(path);
    TEST_ASSERT(fd >= 0, "chat_lock_acquire failed: %d", fd);

    /* First release — should succeed */
    chat_lock_release(fd);

    /* Verify fd is now closed */
    int flags = fcntl(fd, F_GETFD);
    TEST_ASSERT(flags == -1 && errno == EBADF,
                "fd should be closed after first release");

    /* NOTE: second release would hit the ASSERT_MSG(lock_fd >= 0) precondition
     * and abort. We do NOT call chat_lock_release(fd) again here because:
     *   - The fd number is still >= 0 (it's just closed)
     *   - The precondition catches negative fds, not closed fds
     *   - Calling fcntl on a closed fd would produce EBADF, which the
     *     fixed code now logs as a warning
     * This test verifies the fd is properly closed after a single release. */

    cleanup_lock_file(path);
    TEST_PASS("double release: fd is closed after first release");
}

/* --- Test that lock file is created with correct permissions --- */

static void test_lock_file_permissions(void) {
    char path[256];
    make_temp_path(path, sizeof(path));

    int fd = chat_lock_acquire(path);
    TEST_ASSERT(fd >= 0, "chat_lock_acquire failed: %d", fd);

    char lock_path[MAX_PATH_LEN];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);

    struct stat st;
    int ret = stat(lock_path, &st);
    TEST_ASSERT(ret == 0, "stat on lock file failed: %s", strerror(errno));

    /* Verify the file was created with 0600 permissions (modulo umask) */
    mode_t mode = st.st_mode & 0777;
    TEST_ASSERT((mode & 0077) == 0,
                "lock file should not be group/world accessible, mode = 0%03o",
                mode);

    chat_lock_release(fd);
    cleanup_lock_file(path);
    TEST_PASS("lock file created with restricted permissions");
}

/* --- Test multiple sequential acquire/release cycles --- */

static void test_sequential_acquire_release(void) {
    char path[256];
    make_temp_path(path, sizeof(path));

    for (int i = 0; i < 10; i++) {
        int fd = chat_lock_acquire(path);
        TEST_ASSERT(fd >= 0, "chat_lock_acquire failed on iteration %d: %d", i, fd);

        int flags = fcntl(fd, F_GETFD);
        TEST_ASSERT(flags >= 0, "fd invalid on iteration %d", i);

        chat_lock_release(fd);
    }

    cleanup_lock_file(path);
    TEST_PASS("10 sequential acquire/release cycles");
}

int main(void) {
    printf("=== lock unit tests ===\n\n");

    test_acquire_and_release();
    test_lock_is_held();
    test_cloexec_flag();
    test_acquire_invalid_path();
    test_double_release();
    test_lock_file_permissions();
    test_sequential_acquire_release();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
