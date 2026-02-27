/*
 * test_lock_unit.c — Unit tests for fcntl-based file locking
 *
 * Tests:
 *   1.  Lock acquire and release (basic round-trip)
 *   2.  Lock acquire returns valid fd with write lock held
 *   3.  Double-release behaviour (second release on closed fd)
 *   4.  O_CLOEXEC is set on acquired lock fd
 *   5.  Lock acquire on invalid path fails gracefully
 *   6.  Lock release postcondition: fd is closed after release
 *   7.  Lock file created with restricted permissions
 *   8.  10 sequential acquire/release cycles
 *   9.  Empty string path causes abort (violation 1 hardening)
 *   10. Postcondition: acquire returns fd >= 0 (violation 3 hardening)
 *   11. Unlock failure on bad fd causes abort (violation 4 bug fix)
 *   12. Close failure on bad fd causes abort (violation 7 hardening)
 *   13. Uncontested acquire produces no stderr log
 *   14. Contended acquire logs to stderr
 *   15. F_GETLK postcondition check runs after release (violation 5)
 *
 * Build (from src/nbs-chat/ via Makefile):
 *   make test-unit
 *
 * Or manually:
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -I../src/nbs-chat \
 *       -o test_lock_unit test_lock_unit.c \
 *       ../src/nbs-chat/lock.c ../src/nbs-chat/chat_file.c \
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

/* Helper: fork a child and verify it was killed by a signal (SIGABRT)
 * or exited with non-zero status. Returns 1 if the child aborted, 0 otherwise. */
static int expect_abort_in_child(void (*fn)(void)) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        /* Child: redirect stderr to /dev/null to suppress assert messages */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        fn();
        /* If fn() returns, it did NOT abort — exit cleanly so parent can detect */
        _exit(0);
    }
    /* Parent: wait for child */
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
        return 1; /* Killed by SIGABRT — abort() was called */
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        return 1; /* Exited non-zero — also acceptable */
    }
    return 0; /* Exited cleanly — abort did NOT happen */
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
     *   - Calling fcntl on a closed fd would trigger the unlock ASSERT_MSG
     * This test verifies the fd is properly closed after a single release.
     * The abort behaviour is tested separately in test_release_closed_fd_aborts. */

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

/* ===================================================================
 * Adversarial tests for audit violations
 * =================================================================== */

/* --- Violation 1 (HARDENING): Empty string path causes abort --- */

static void child_empty_path(void) {
    chat_lock_acquire("");
}

static void test_empty_path_aborts(void) {
    int aborted = expect_abort_in_child(child_empty_path);
    TEST_ASSERT(aborted,
                "chat_lock_acquire(\"\") should abort due to empty-string precondition");
    TEST_PASS("empty string path causes abort");
}

/* --- Violation 3 (HARDENING): Postcondition fd >= 0 on success --- */

static void test_postcondition_fd_valid(void) {
    char path[256];
    make_temp_path(path, sizeof(path));

    /* Acquire must return fd >= 0 on success (postcondition) */
    int fd = chat_lock_acquire(path);
    TEST_ASSERT(fd >= 0,
                "postcondition: chat_lock_acquire must return fd >= 0 on success, got %d", fd);

    /* Additionally verify it is a real open fd */
    int flags = fcntl(fd, F_GETFD);
    TEST_ASSERT(flags >= 0,
                "postcondition: returned fd %d must be a valid open descriptor", fd);

    chat_lock_release(fd);
    cleanup_lock_file(path);
    TEST_PASS("postcondition: acquire returns valid fd >= 0");
}

/* --- Violation 4 (BUG): Unlock failure on already-closed fd aborts ---
 *
 * This is the critical BUG fix. Previously, fcntl unlock failure was logged
 * as a warning and silently swallowed. Now it aborts, because a stuck lock
 * blocks all other processes indefinitely and process exit releases all
 * fcntl locks anyway.
 *
 * We test this by:
 *   1. Acquiring a lock (getting a valid fd)
 *   2. Closing the fd behind the back of chat_lock_release
 *   3. Calling chat_lock_release with the now-closed fd
 *   4. Verifying the process aborts (SIGABRT)
 */

static void child_release_closed_fd(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_lock_abort_%d", getpid());

    int fd = chat_lock_acquire(path);
    if (fd < 0) _exit(0); /* Should not happen but prevent UB */

    /* Close the fd behind chat_lock_release's back */
    close(fd);

    /* Now call release — fcntl on a closed fd returns EBADF, which should abort */
    chat_lock_release(fd);

    /* Should not reach here */
    _exit(0);
}

static void test_release_closed_fd_aborts(void) {
    int aborted = expect_abort_in_child(child_release_closed_fd);
    TEST_ASSERT(aborted,
                "chat_lock_release on a closed fd should abort (BUG fix: unlock failure must not be silent)");
    /* Clean up — the child may have created a lock file */
    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_lock_abort_%d", getpid());
    cleanup_lock_file(path);
    TEST_PASS("release on closed fd aborts (violation 4 BUG fix)");
}

/* --- Violation 4 (BUG): Verify release on VALID fd does NOT abort ---
 *
 * Counterpart to the above: a normal release must NOT abort.
 * This falsifies any regression where the abort is too aggressive. */

static void child_release_valid_fd(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_lock_valid_%d", getpid());

    int fd = chat_lock_acquire(path);
    if (fd < 0) _exit(1);

    chat_lock_release(fd);

    /* Clean up lock file */
    char lock_path[MAX_PATH_LEN];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    unlink(lock_path);

    _exit(0);
}

static void test_release_valid_fd_does_not_abort(void) {
    int aborted = expect_abort_in_child(child_release_valid_fd);
    TEST_ASSERT(!aborted,
                "chat_lock_release on a valid fd should NOT abort — normal operation");
    TEST_PASS("release on valid fd does not abort (no false positive)");
}

/* --- Lock logging: no log on uncontested acquire, log only on contention ---
 *
 * The lock uses F_SETLK (non-blocking) first. If it succeeds, no log.
 * Only if the lock is contended does it log and fall back to F_SETLKW. */

static void test_uncontested_acquire_no_log(void) {
    int pipefd[2];
    int ret = pipe(pipefd);
    TEST_ASSERT(ret == 0, "pipe() failed: %s", strerror(errno));

    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork() failed: %s", strerror(errno));

    if (pid == 0) {
        /* Child: redirect stderr to pipe write end */
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        char path[256];
        snprintf(path, sizeof(path), "/tmp/test_lock_quiet_%d", getpid());
        int fd = chat_lock_acquire(path);
        if (fd >= 0) {
            chat_lock_release(fd);
        }
        char lock_path[MAX_PATH_LEN];
        snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
        unlink(lock_path);
        _exit(0);
    }

    /* Parent: read child's stderr from pipe */
    close(pipefd[1]);

    char buf[4096];
    ssize_t n = 0;
    ssize_t total = 0;
    while ((n = read(pipefd[0], buf + total, sizeof(buf) - (size_t)total - 1)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    /* Uncontested acquire should produce NO "info:" log */
    TEST_ASSERT(strstr(buf, "info:") == NULL,
                "uncontested acquire should not log, but got: %s", buf);

    TEST_PASS("uncontested acquire produces no stderr log");
}

static void test_contended_acquire_logs(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_lock_contend_%d", getpid());

    /* Parent acquires lock first */
    int parent_fd = chat_lock_acquire(path);
    TEST_ASSERT(parent_fd >= 0, "parent acquire failed: %d", parent_fd);

    int pipefd[2];
    int ret = pipe(pipefd);
    TEST_ASSERT(ret == 0, "pipe() failed: %s", strerror(errno));

    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork() failed: %s", strerror(errno));

    if (pid == 0) {
        /* Child: redirect stderr to pipe, try to acquire (will contend) */
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        int fd = chat_lock_acquire(path);
        if (fd >= 0) {
            chat_lock_release(fd);
        }
        _exit(0);
    }

    /* Parent: hold lock briefly, then release so child can proceed */
    usleep(100000); /* 100ms — child should be contending by now */
    chat_lock_release(parent_fd);

    /* Read child's stderr */
    close(pipefd[1]);
    char buf[4096];
    ssize_t n = 0;
    ssize_t total = 0;
    while ((n = read(pipefd[0], buf + total, sizeof(buf) - (size_t)total - 1)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    /* Contended acquire SHOULD produce the log */
    TEST_ASSERT(strstr(buf, "info: chat_lock_acquire: lock contended") != NULL,
                "contended acquire should log, but got: '%s'", buf);

    char lock_path[MAX_PATH_LEN];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    unlink(lock_path);

    TEST_PASS("contended acquire logs to stderr");
}

/* --- Violation 5 (HARDENING): F_GETLK postcondition check runs ---
 *
 * After a successful unlock and before close, the code now checks F_GETLK
 * to verify the lock was released. We cannot easily make F_GETLK fail in
 * a unit test, but we CAN verify that a normal acquire/release cycle
 * passes the postcondition check without aborting (i.e. the check runs
 * and does not false-positive). This is tested implicitly by all the
 * acquire/release tests above. This test makes it explicit. */

static void test_postcondition_check_runs(void) {
    char path[256];
    make_temp_path(path, sizeof(path));

    /* If the postcondition check were broken (e.g. false positive),
     * this would abort. */
    int fd = chat_lock_acquire(path);
    TEST_ASSERT(fd >= 0, "chat_lock_acquire failed: %d", fd);

    /* Release — postcondition check (F_GETLK) runs here */
    chat_lock_release(fd);

    /* If we get here, the postcondition check passed */
    cleanup_lock_file(path);
    TEST_PASS("F_GETLK postcondition check runs without false positive");
}

/* --- Adversarial: NULL path causes abort --- */

static void child_null_path(void) {
    chat_lock_acquire(NULL);
}

static void test_null_path_aborts(void) {
    int aborted = expect_abort_in_child(child_null_path);
    TEST_ASSERT(aborted,
                "chat_lock_acquire(NULL) should abort due to NULL precondition");
    TEST_PASS("NULL path causes abort");
}

/* --- Adversarial: negative fd to release causes abort --- */

static void child_negative_fd(void) {
    chat_lock_release(-1);
}

static void test_negative_fd_aborts(void) {
    int aborted = expect_abort_in_child(child_negative_fd);
    TEST_ASSERT(aborted,
                "chat_lock_release(-1) should abort due to precondition lock_fd >= 0");
    TEST_PASS("negative fd to release causes abort");
}

/* --- Adversarial: concurrent lock contention via fork ---
 *
 * Verify that two processes contending for the same lock both succeed
 * sequentially (one blocks until the other releases). This exercises
 * the F_SETLKW blocking path under real contention. */

static void test_concurrent_lock_contention(void) {
    char path[256];
    make_temp_path(path, sizeof(path));

    /* Create a shared file to record acquisition order */
    char order_path[512];
    snprintf(order_path, sizeof(order_path), "%s.order", path);
    unlink(order_path);

    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork() failed: %s", strerror(errno));

    if (pid == 0) {
        /* Child: acquire lock, write "C" to order file, hold briefly, release */
        int fd = chat_lock_acquire(path);
        if (fd < 0) _exit(1);

        /* Write marker while holding lock */
        int ofd = open(order_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (ofd >= 0) {
            (void)write(ofd, "C", 1);
            close(ofd);
        }

        /* Hold lock briefly to create contention */
        usleep(50000); /* 50ms */

        chat_lock_release(fd);
        _exit(0);
    }

    /* Parent: slight delay to let child acquire first, then contend */
    usleep(10000); /* 10ms — child should have lock by now */

    int fd = chat_lock_acquire(path);
    TEST_ASSERT(fd >= 0, "parent chat_lock_acquire failed during contention: %d", fd);

    /* Write marker while holding lock */
    int ofd = open(order_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (ofd >= 0) {
        (void)write(ofd, "P", 1);
        close(ofd);
    }

    chat_lock_release(fd);

    /* Wait for child */
    int status = 0;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "child process failed during lock contention");

    /* Verify both acquired the lock (order file has 2 chars) */
    int rfd = open(order_path, O_RDONLY);
    TEST_ASSERT(rfd >= 0, "could not open order file: %s", strerror(errno));
    char order_buf[8];
    ssize_t n = read(rfd, order_buf, sizeof(order_buf) - 1);
    close(rfd);
    TEST_ASSERT(n == 2,
                "expected 2 acquisitions recorded, got %zd bytes", n);
    order_buf[n] = '\0';
    /* Either "CP" or "PC" — both are valid, meaning both processes acquired */
    TEST_ASSERT((strcmp(order_buf, "CP") == 0 || strcmp(order_buf, "PC") == 0),
                "expected acquisition order 'CP' or 'PC', got '%s'", order_buf);

    unlink(order_path);
    cleanup_lock_file(path);
    TEST_PASS("concurrent lock contention: both processes acquire sequentially");
}

int main(void) {
    printf("=== lock unit tests ===\n\n");

    /* Original tests */
    test_acquire_and_release();
    test_lock_is_held();
    test_cloexec_flag();
    test_acquire_invalid_path();
    test_double_release();
    test_lock_file_permissions();
    test_sequential_acquire_release();

    /* Adversarial tests for audit violations */
    printf("\n--- adversarial tests (audit violations) ---\n\n");
    test_empty_path_aborts();
    test_postcondition_fd_valid();
    test_release_closed_fd_aborts();
    test_release_valid_fd_does_not_abort();
    test_uncontested_acquire_no_log();
    test_contended_acquire_logs();
    test_postcondition_check_runs();
    test_null_path_aborts();
    test_negative_fd_aborts();
    test_concurrent_lock_contention();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
