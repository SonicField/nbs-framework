/*
 * lock.h — File locking for nbs-chat
 *
 * Uses fcntl F_SETLKW for POSIX advisory locking on companion .lock files.
 * Lock is held for the duration of a read-modify-write cycle.
 */

#ifndef NBS_LOCK_H
#define NBS_LOCK_H

/*
 * chat_lock_acquire — Acquire exclusive advisory lock on chat file.
 *
 * Opens/creates the companion .lock file and acquires an exclusive
 * POSIX advisory lock via fcntl F_SETLKW. Advisory locks are cooperative;
 * all processes must use this function for exclusion to be effective.
 *
 * WARNING: Advisory lock correctness is an unverifiable assumption.
 * There is no mechanism to detect or prevent direct file access
 * bypassing this lock. All chat file accessors MUST use this API.
 *
 * Preconditions:
 *   - chat_path != NULL
 *   - strlen(chat_path) + 6 <= MAX_PATH_LEN
 *
 * Postconditions (on success):
 *   - Returned fd >= 0 with exclusive advisory lock held
 *   - Caller MUST call chat_lock_release with the returned fd
 *
 * Returns:
 *   - fd >= 0 on success (lock held)
 *   - -1 on error (no lock held, no cleanup needed)
 */
int chat_lock_acquire(const char *chat_path);

/*
 * chat_lock_release — Release exclusive advisory lock.
 *
 * Releases the POSIX advisory lock and closes the lock file descriptor.
 * Advisory locks are automatically released on close, but we explicitly
 * unlock first for clarity and to avoid relying on implicit behaviour.
 *
 * Preconditions:
 *   - lock_fd >= 0 (a valid fd returned by chat_lock_acquire)
 *
 * Postconditions:
 *   - The advisory lock is released
 *   - lock_fd is closed and must not be reused
 */
void chat_lock_release(int lock_fd);

#endif /* NBS_LOCK_H */
