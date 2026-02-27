/*
 * lock.c — fcntl-based file locking
 */

#include "lock.h"
#include "chat_file.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int chat_lock_acquire(const char *chat_path) {
    ASSERT_MSG(chat_path != NULL, "chat_lock_acquire: path is NULL");
    ASSERT_MSG(chat_path[0] != '\0',
               "chat_lock_acquire: path is empty string, expected a non-empty chat file path");

    /* Build lock file path: chat_path + ".lock" */
    size_t path_len = strlen(chat_path);
    ASSERT_MSG(path_len + 6 <= MAX_PATH_LEN,
               "chat_lock_acquire: path too long: %zu + 6 > %d", path_len, MAX_PATH_LEN);
    char lock_path[MAX_PATH_LEN];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", chat_path);

    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "warning: chat_lock_acquire: open failed for %s: %s\n",
                lock_path, strerror(errno));
        return -1;
    }

    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0, /* Lock entire file */
    };

    /* Block until lock is acquired.
     * F_SETLKW blocks indefinitely — this is intentional. Lock contention
     * is expected to be brief (held only for read-modify-write cycles).
     * A timeout is not used because POSIX fcntl does not support one, and
     * alarm-based interruption would complicate error handling for minimal
     * benefit given the expected short hold times.
     *
     * Log before blocking so that stalls are diagnosable. */
    fprintf(stderr, "info: chat_lock_acquire: waiting for lock on %s (pid %d)\n",
            lock_path, (int)getpid());
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        fprintf(stderr, "warning: chat_lock_acquire: fcntl lock failed for %s: %s\n",
                lock_path, strerror(errno));
        close(fd);
        return -1;
    }

    /* Postcondition: fd is valid and lock is held */
    ASSERT_MSG(fd >= 0,
               "chat_lock_acquire: postcondition violated — fd is %d after successful lock", fd);

    return fd;
}

void chat_lock_release(int lock_fd) {
    ASSERT_MSG(lock_fd >= 0, "chat_lock_release: invalid fd %d", lock_fd);

    struct flock fl = {
        .l_type = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };

    int unlock_ret = fcntl(lock_fd, F_SETLK, &fl);
    /* Unlock failure means the lock state is uncertain. A stuck lock blocks
     * all other processes indefinitely. Aborting is safer than continuing:
     * process exit releases all fcntl locks held by this process. */
    ASSERT_MSG(unlock_ret == 0,
               "chat_lock_release: fcntl unlock failed for fd %d: %s — aborting to release locks",
               lock_fd, strerror(errno));

    /* Postcondition: verify the lock was actually released.
     *
     * F_GETLK reports who WOULD block an acquisition attempt. After we
     * unlock, another process may immediately acquire the lock. In that
     * case F_GETLK reports F_WRLCK with that process's PID — not ours.
     * We only assert failure if the lock is still held by THIS process,
     * which would indicate the unlock silently failed. */
    struct flock check = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    if (fcntl(lock_fd, F_GETLK, &check) == 0) {
        ASSERT_MSG(check.l_type == F_UNLCK || check.l_pid != getpid(),
                   "chat_lock_release: lock still held by us after unlock on fd %d", lock_fd);
    } else {
        fprintf(stderr, "warning: chat_lock_release: F_GETLK postcondition check failed for fd %d: %s\n",
                lock_fd, strerror(errno));
    }

    int close_ret = close(lock_fd);
    ASSERT_MSG(close_ret == 0,
               "chat_lock_release: close failed for fd %d: %s — fd leak", lock_fd, strerror(errno));
}
