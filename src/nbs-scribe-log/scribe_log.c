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
    if (fd < 0) return;

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

int scribe_log_init(const char *log_path)
{
    ASSERT_MSG(log_path != NULL, "scribe_log_init: log_path is NULL");

    /* Check if file already exists */
    if (access(log_path, F_OK) == 0)
        return 0;

    /* Create parent directories if needed */
    char dir[SCRIBE_MAX_PATH];
    int n = snprintf(dir, sizeof(dir), "%s", log_path);
    ASSERT_MSG(n > 0 && (size_t)n < sizeof(dir),
               "scribe_log_init: path too long");

    /* Find last slash to get directory */
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        /* Simple mkdir -p: create each component */
        for (char *p = dir + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(dir, 0755);
                *p = '/';
            }
        }
        mkdir(dir, 0755);
    }

    /* Get current ISO 8601 timestamp */
    time_t now = time(NULL);
    ASSERT_MSG(now != (time_t)-1, "scribe_log_init: time() failed");
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

    FILE *f = fopen(log_path, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot create log file %s: %s\n",
                log_path, strerror(errno));
        return -1;
    }

    int write_err = 0;
    if (fprintf(f, "# Decision Log\n\n") < 0) write_err = 1;
    if (fprintf(f, "Created: %s\n", ts) < 0) write_err = 1;
    if (fprintf(f, "Decision count: 0\n\n") < 0) write_err = 1;
    if (fprintf(f, "---\n") < 0) write_err = 1;

    if (fclose(f) != 0) write_err = 1;

    if (write_err) {
        fprintf(stderr, "Error: write failed creating %s\n", log_path);
        unlink(log_path);
        return -1;
    }

    return 0;
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

    /* Initialise log if it does not exist */
    if (access(log_path, F_OK) != 0) {
        if (scribe_log_init(log_path) != 0)
            return SCRIBE_EXIT_ERROR;
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

    /* Acquire lock */
    int lock_fd = lock_acquire(log_path);
    if (lock_fd < 0)
        return SCRIBE_EXIT_ERROR;

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
    snprintf(bus_payload, sizeof(bus_payload), "D-%lld %s", ts, entry->summary);

    const char *bus_dir = entry->bus_dir[0] ? entry->bus_dir : ".nbs/events/";
    bus_publish(bus_dir, bus_payload);

    /* Print decision ID to stdout */
    printf("D-%lld\n", ts);

    return SCRIBE_EXIT_OK;
}
