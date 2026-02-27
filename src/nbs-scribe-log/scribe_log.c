/*
 * scribe_log.c — Decision log append, validation, and bus event publishing.
 *
 * Entry format (appended to log file):
 *
 *   ---
 *
 *   ### D-<unix-timestamp> <summary>
 *   - **Chat ref:** <chat-ref>
 *   - **Participants:** <participants>
 *   - **Artefacts:** <artefacts>
 *   - **Risk tags:** <risk-tags>
 *   - **Status:** <status>
 *   - **Rationale:** <rationale>
 *
 * Locking: fcntl F_SETLKW on <log_path>.lock. Same pattern as nbs-chat.
 * Bus publish: fork+exec of nbs-bus binary. Failure is non-fatal.
 */

#include "scribe_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Directory creation                                                  */
/* ------------------------------------------------------------------ */

/*
 * ensure_parent_dirs — Create parent directories for a file path.
 *
 * Idempotent: safe to call concurrently or repeatedly. Uses mkdir
 * and ignores EEXIST.
 *
 * Returns SCRIBE_EXIT_OK on success, SCRIBE_EXIT_ERROR on failure.
 */
static int ensure_parent_dirs(const char *file_path)
{
    ASSERT_MSG(file_path != NULL, "ensure_parent_dirs: file_path is NULL");

    char dir[SCRIBE_MAX_PATH];
    int n = snprintf(dir, sizeof(dir), "%s", file_path);
    ASSERT_MSG(n > 0 && (size_t)n < sizeof(dir),
               "ensure_parent_dirs: path too long");

    char *slash = strrchr(dir, '/');
    if (!slash) return SCRIBE_EXIT_OK; /* No directory component */

    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            /* HARDENING: Check mkdir errors beyond EEXIST */
            if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "Error: cannot create directory %s: %s\n",
                        dir, strerror(errno));
                return SCRIBE_EXIT_ERROR;
            }
            *p = '/';
        }
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: cannot create directory %s: %s\n",
                dir, strerror(errno));
        return SCRIBE_EXIT_ERROR;
    }

    return SCRIBE_EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* Locking (adapted from src/nbs-chat/lock.c)                          */
/* ------------------------------------------------------------------ */

static int lock_acquire(const char *log_path)
{
    ASSERT_MSG(log_path != NULL, "lock_acquire: log_path is NULL");

    char lock_path[SCRIBE_MAX_PATH];
    int n = snprintf(lock_path, sizeof(lock_path), "%s.lock", log_path);
    ASSERT_MSG(n > 0 && (size_t)n < sizeof(lock_path),
               "lock_acquire: lock path too long");

    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open lock file %s: %s\n",
                lock_path, strerror(errno));
        return -1;
    }

    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };

    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        fprintf(stderr, "Error: cannot acquire lock on %s: %s\n",
                lock_path, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void lock_release(int fd)
{
    /* HARDENING: A negative fd indicates a logic error in the caller —
     * lock_release should only be called after a successful lock_acquire. */
    ASSERT_MSG(fd >= 0, "lock_release: called with invalid fd %d", fd);

    struct flock fl = {
        .l_type = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };

    if (fcntl(fd, F_SETLK, &fl) < 0) {
        fprintf(stderr, "Warning: unlock failed: %s\n", strerror(errno));
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* Bus publish (fork+exec nbs-bus)                                     */
/* ------------------------------------------------------------------ */

static void bus_publish(const char *bus_dir, const char *payload)
{
    if (!bus_dir || bus_dir[0] == '\0') return;

    /* BUG: payload must be validated — NULL payload would cause UB in execlp */
    ASSERT_MSG(payload != NULL, "bus_publish: payload is NULL");

    /* Check bus directory exists */
    struct stat st;
    if (stat(bus_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Warning: bus directory %s not found, "
                "skipping event publish\n", bus_dir);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Warning: fork failed for bus publish: %s\n",
                strerror(errno));
        return;
    }

    if (pid == 0) {
        /* Child: redirect stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execlp("nbs-bus", "nbs-bus", "publish", bus_dir,
               "scribe", "decision-logged", "normal", payload, NULL);
        _exit(127);
    }

    /* Parent: wait for child */
    int wstatus;
    waitpid(pid, &wstatus, 0);
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        fprintf(stderr, "Warning: nbs-bus publish failed (exit %d)\n",
                WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1);
    }
}

/* ------------------------------------------------------------------ */
/* Log initialisation                                                  */
/* ------------------------------------------------------------------ */

/*
 * write_log_header — Atomically create a new log file with header.
 *
 * Uses O_CREAT|O_EXCL for atomic create-or-fail, eliminating the
 * TOCTOU race from the original access()+fopen("w") pattern.
 * Parent directories must already exist (call ensure_parent_dirs first).
 *
 * Returns SCRIBE_EXIT_OK (0) on success or if file already exists.
 * Returns SCRIBE_EXIT_ERROR (1) on error.
 */
static int write_log_header(const char *log_path)
{
    ASSERT_MSG(log_path != NULL, "write_log_header: log_path is NULL");

    /* BUG FIX (TOCTOU): Use O_CREAT|O_EXCL for atomic create-or-fail.
     * This eliminates the race where two processes both see "file doesn't
     * exist" and one truncates the other's header. */
    /* SECURITY: File created with explicit mode 0644 via open(),
     * not dependent on umask like fopen(). */
    int fd = open(log_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            /* File already exists — not an error */
            return SCRIBE_EXIT_OK;
        }
        fprintf(stderr, "Error: cannot create log file %s: %s\n",
                log_path, strerror(errno));
        return SCRIBE_EXIT_ERROR;
    }

    FILE *f = fdopen(fd, "w");
    if (!f) {
        fprintf(stderr, "Error: fdopen failed for %s: %s\n",
                log_path, strerror(errno));
        close(fd);
        unlink(log_path);
        return SCRIBE_EXIT_ERROR;
    }

    /* Get current ISO 8601 timestamp */
    time_t now = time(NULL);
    ASSERT_MSG(now != (time_t)-1, "write_log_header: time() failed");
    struct tm tm;
    /* BUG FIX: gmtime_r return value must be checked */
    struct tm *tmresult = gmtime_r(&now, &tm);
    ASSERT_MSG(tmresult != NULL, "write_log_header: gmtime_r failed");
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

    int write_err = 0;
    if (fprintf(f, "# Decision Log\n\n") < 0) write_err = 1;
    if (fprintf(f, "Created: %s\n", ts) < 0) write_err = 1;
    if (fprintf(f, "Decision count: 0\n\n") < 0) write_err = 1;
    if (fprintf(f, "---\n") < 0) write_err = 1;

    if (fclose(f) != 0) write_err = 1;

    if (write_err) {
        fprintf(stderr, "Error: write failed creating %s\n", log_path);
        unlink(log_path);
        return SCRIBE_EXIT_ERROR;
    }

    return SCRIBE_EXIT_OK;
}

int scribe_log_init(const char *log_path)
{
    ASSERT_MSG(log_path != NULL, "scribe_log_init: log_path is NULL");

    /* Check if file already exists */
    if (access(log_path, F_OK) == 0)
        return SCRIBE_EXIT_OK;

    /* Create parent directories (idempotent) */
    int drc = ensure_parent_dirs(log_path);
    if (drc != SCRIBE_EXIT_OK)
        return SCRIBE_EXIT_ERROR;

    /* HARDENING: Consistent return value contract — use SCRIBE_EXIT_ERROR
     * (1) on error, not -1, matching scribe_log_append's convention. */
    return write_log_header(log_path);
}

/* ------------------------------------------------------------------ */
/* Entry append                                                        */
/* ------------------------------------------------------------------ */

int scribe_log_append(const char *log_path, const scribe_entry_t *entry)
{
    ASSERT_MSG(log_path != NULL, "scribe_log_append: log_path is NULL");
    ASSERT_MSG(entry != NULL, "scribe_log_append: entry is NULL");
    ASSERT_MSG(entry->summary[0] != '\0',
               "scribe_log_append: summary is empty");
    ASSERT_MSG(entry->participants[0] != '\0',
               "scribe_log_append: participants is empty");
    ASSERT_MSG(entry->rationale[0] != '\0',
               "scribe_log_append: rationale is empty");

    /* SECURITY: Reject newlines in all fields written to single Markdown lines.
     * This prevents injection of fake decision entries into the log. */
    ASSERT_MSG(strchr(entry->summary, '\n') == NULL,
               "scribe_log_append: summary contains newline (injection risk)");
    ASSERT_MSG(strchr(entry->participants, '\n') == NULL,
               "scribe_log_append: participants contains newline (injection risk)");
    ASSERT_MSG(strchr(entry->rationale, '\n') == NULL,
               "scribe_log_append: rationale contains newline (injection risk)");
    if (entry->chat_ref[0] != '\0') {
        ASSERT_MSG(strchr(entry->chat_ref, '\n') == NULL,
                   "scribe_log_append: chat_ref contains newline (injection risk)");
    }
    if (entry->artefacts[0] != '\0') {
        ASSERT_MSG(strchr(entry->artefacts, '\n') == NULL,
                   "scribe_log_append: artefacts contains newline (injection risk)");
    }
    if (entry->risk_tags[0] != '\0') {
        ASSERT_MSG(strchr(entry->risk_tags, '\n') == NULL,
                   "scribe_log_append: risk_tags contains newline (injection risk)");
    }
    if (entry->supersedes[0] != '\0') {
        ASSERT_MSG(strchr(entry->supersedes, '\n') == NULL,
                   "scribe_log_append: supersedes contains newline (injection risk)");
    }

    /* Create parent directories before acquiring the lock.
     * Directory creation is idempotent and must happen first because
     * lock_acquire needs the parent directory to exist for the lock file. */
    int drc = ensure_parent_dirs(log_path);
    if (drc != SCRIBE_EXIT_OK)
        return SCRIBE_EXIT_ERROR;

    /* BUG FIX (TOCTOU): Acquire lock BEFORE checking file existence.
     * The original code checked access() then acquired the lock, allowing
     * a race where two processes both see "file doesn't exist" and one
     * truncates the other's init header. */
    int lock_fd = lock_acquire(log_path);
    if (lock_fd < 0)
        return SCRIBE_EXIT_ERROR;

    /* Initialise log if it does not exist (now under lock).
     * write_log_header uses O_CREAT|O_EXCL for atomic create-or-fail,
     * so even if access() has a tiny TOCTOU window with the lock, the
     * actual file creation is atomic. */
    if (access(log_path, F_OK) != 0) {
        int init_rc = write_log_header(log_path);
        if (init_rc != SCRIBE_EXIT_OK) {
            lock_release(lock_fd);
            return SCRIBE_EXIT_ERROR;
        }
    }

    /* Generate timestamp */
    time_t now = time(NULL);
    ASSERT_MSG(now != (time_t)-1, "scribe_log_append: time() failed");
    long long ts = (long long)now;

    /* Build the entry text */
    char entry_text[SCRIBE_MAX_ENTRY];
    const char *status = entry->status[0] ? entry->status : "decided";
    const char *artefacts = entry->artefacts[0] ? entry->artefacts : "\xe2\x80\x94"; /* em-dash */
    const char *risk_tags = entry->risk_tags[0] ? entry->risk_tags : "none";
    const char *chat_ref = entry->chat_ref[0] ? entry->chat_ref : "\xe2\x80\x94";

    /* Build summary line, optionally with [SUPERSEDES D-xxx] prefix */
    char summary_line[SCRIBE_MAX_SUMMARY + 64];
    if (entry->supersedes[0]) {
        int sn = snprintf(summary_line, sizeof(summary_line),
                 "[SUPERSEDES %s] %s", entry->supersedes, entry->summary);
        ASSERT_MSG(sn > 0 && (size_t)sn < sizeof(summary_line),
                   "scribe_log_append: summary line too long");
    } else {
        int sn = snprintf(summary_line, sizeof(summary_line),
                 "%s", entry->summary);
        ASSERT_MSG(sn > 0 && (size_t)sn < sizeof(summary_line),
                   "scribe_log_append: summary line too long");
    }

    int n = snprintf(entry_text, sizeof(entry_text),
                     "\n---\n\n"
                     "### D-%lld %s\n"
                     "- **Chat ref:** %s\n"
                     "- **Participants:** %s\n"
                     "- **Artefacts:** %s\n"
                     "- **Risk tags:** %s\n"
                     "- **Status:** %s\n"
                     "- **Rationale:** %s\n",
                     ts, summary_line,
                     chat_ref,
                     entry->participants,
                     artefacts,
                     risk_tags,
                     status,
                     entry->rationale);

    ASSERT_MSG(n > 0 && (size_t)n < sizeof(entry_text),
               "scribe_log_append: entry too long (%d chars)", n);

    /* Append to log file */
    FILE *f = fopen(log_path, "a");
    if (!f) {
        fprintf(stderr, "Error: cannot open log file %s: %s\n",
                log_path, strerror(errno));
        lock_release(lock_fd);
        return SCRIBE_EXIT_ERROR;
    }

    size_t entry_len = strlen(entry_text);
    size_t written = fwrite(entry_text, 1, entry_len, f);
    if (fclose(f) != 0 || written != entry_len) {
        fprintf(stderr, "Error: write failed to %s\n", log_path);
        lock_release(lock_fd);
        return SCRIBE_EXIT_ERROR;
    }

    lock_release(lock_fd);

    /* Publish bus event (non-fatal if this fails) */
    char bus_payload[SCRIBE_MAX_SUMMARY + 32];
    /* HARDENING: Check snprintf return for bus_payload truncation */
    int bp = snprintf(bus_payload, sizeof(bus_payload),
                      "D-%lld %s", ts, entry->summary);
    ASSERT_MSG(bp > 0 && (size_t)bp < sizeof(bus_payload),
               "scribe_log_append: bus_payload truncated");

    /* NOTE: Default bus directory ".nbs/events/" is relative to cwd.
     * This is intentional — the tool is designed to be run from within
     * a project directory where .nbs/ is the project's NBS root. */
    const char *bus_dir = entry->bus_dir[0] ? entry->bus_dir : ".nbs/events/";
    bus_publish(bus_dir, bus_payload);

    /* Print decision ID to stdout */
    printf("D-%lld\n", ts);

    return SCRIBE_EXIT_OK;
}
