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

    /* POSTCONDITION: Parent directory must exist and be a directory.
     * This makes the "idempotent" claim falsifiable — if the directory
     * does not exist after our work, the claim is violated. */
    struct stat dir_st;
    ASSERT_MSG(stat(dir, &dir_st) == 0 && S_ISDIR(dir_st.st_mode),
               "ensure_parent_dirs: postcondition failed — '%s' is not a directory "
               "after creation attempt", dir);

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

    /* HARDENING: Removed redundant access() pre-check (TOCTOU).
     * write_log_header uses O_CREAT|O_EXCL for atomic create-or-fail,
     * returning SCRIBE_EXIT_OK if the file already exists (EEXIST).
     * The access() call was redundant and introduced a TOCTOU window. */

    /* Create parent directories (idempotent) */
    int drc = ensure_parent_dirs(log_path);
    if (drc != SCRIBE_EXIT_OK)
        return SCRIBE_EXIT_ERROR;

    /* HARDENING: Consistent return value contract — use SCRIBE_EXIT_ERROR
     * (1) on error, not -1, matching scribe_log_append's convention. */
    return write_log_header(log_path);
}

/* ------------------------------------------------------------------ */
/* Auto-archive                                                        */
/* ------------------------------------------------------------------ */

#define SCRIBE_ARCHIVE_THRESHOLD  500   /* decisions before archive triggers */
#define SCRIBE_ARCHIVE_CLEAVE     250   /* decisions moved to archive */

/*
 * scribe_log_auto_archive — Cleave old decisions into an archive file.
 *
 * Called from scribe_log_append after the successful write, while the
 * fcntl lock is still held. Mirrors the chat_auto_archive pattern.
 *
 * Preconditions:
 *   - Lock is held by caller (do NOT acquire a second lock)
 *   - log_path points to the live log file
 *
 * Postconditions:
 *   - If decision count <= SCRIBE_ARCHIVE_THRESHOLD: no-op
 *   - On success: archive file created with header + first 250 entries;
 *     main file rewritten with updated count + remaining entries
 *   - On failure: warning to stderr, main file unchanged (non-fatal)
 */
static void scribe_log_auto_archive(const char *log_path)
{
    ASSERT_MSG(log_path != NULL, "scribe_log_auto_archive: log_path is NULL");

    /* Read entire file into memory */
    FILE *f = fopen(log_path, "r");
    if (!f) {
        fprintf(stderr, "Warning: auto-archive: cannot open %s: %s\n",
                log_path, strerror(errno));
        return;
    }

    /* Get file size */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "Warning: auto-archive: fseek failed: %s\n",
                strerror(errno));
        return;
    }
    long file_size = ftell(f);
    if (file_size <= 0) {
        fclose(f);
        return;
    }
    rewind(f);

    char *content = malloc((size_t)file_size + 1);
    if (!content) {
        fclose(f);
        fprintf(stderr, "Warning: auto-archive: malloc failed\n");
        return;
    }

    size_t nread = fread(content, 1, (size_t)file_size, f);
    fclose(f);
    content[nread] = '\0';

    /* Count "### D-" lines and record their offsets */
    int decision_count = 0;
    /* First pass: count decisions */
    {
        const char *p = content;
        while ((p = strstr(p, "\n### D-")) != NULL) {
            decision_count++;
            p += 7; /* skip past "\n### D-" */
        }
        /* Also check if file starts with "### D-" (no leading newline) */
        if (strncmp(content, "### D-", 6) == 0)
            decision_count++;
    }

    if (decision_count <= SCRIBE_ARCHIVE_THRESHOLD) {
        free(content);
        return;
    }

    /* Find the byte offset of the (ARCHIVE_CLEAVE + 1)th "### D-" line.
     * Everything before that offset (header + first 250 entries) goes to archive.
     * Everything from that offset onward stays in main. */
    int seen = 0;
    size_t cleave_offset = 0;
    {
        const char *p = content;
        /* Check start-of-file case */
        if (strncmp(content, "### D-", 6) == 0) {
            seen++;
            if (seen > SCRIBE_ARCHIVE_CLEAVE) {
                cleave_offset = 0;
                goto found_cleave;
            }
            p = content + 6;
        }
        while ((p = strstr(p, "\n### D-")) != NULL) {
            seen++;
            if (seen > SCRIBE_ARCHIVE_CLEAVE) {
                /* p points to the '\n' before "### D-" — the cleave point
                 * is at p+1 (start of the "### D-" line itself).
                 * But we also want the preceding "---" separator to stay
                 * with the entry. Scan backward for "---\n" preceding this. */
                cleave_offset = (size_t)(p - content);
                /* Walk back over whitespace/separator to include \n---\n
                 * with the remaining file, not the archive */
                /* Actually, each entry starts with "\n---\n\n### D-".
                 * We want the "---" separator to stay with its entry,
                 * so cleave at the "\n---" before this entry. */
                /* Search backward from p for the preceding "\n---\n" or
                 * "\n\n---\n" that introduces this entry's block */
                const char *scan = p;
                /* Walk backwards over any blank lines */
                while (scan > content && *(scan - 1) == '\n') scan--;
                /* Now check for "---" */
                if (scan >= content + 3 && scan[-3] == '-' && scan[-2] == '-' && scan[-1] == '-') {
                    /* Include the newline before "---" */
                    scan -= 3;
                    if (scan > content && *(scan - 1) == '\n') scan--;
                    cleave_offset = (size_t)(scan - content);
                    if (cleave_offset > 0) cleave_offset++; /* keep trailing \n of archive part */
                }
                goto found_cleave;
            }
            p += 7;
        }
        /* Should not reach here since decision_count > threshold > cleave */
        free(content);
        fprintf(stderr, "Warning: auto-archive: inconsistent decision count\n");
        return;
    }

found_cleave:
    ;

    /* Identify the header: everything before the first "### D-" entry
     * (including the initial "---" separator). The header ends just before
     * the first entry block's separator. */
    size_t header_end = 0;
    {
        /* Find first "### D-" */
        const char *first_entry;
        if (strncmp(content, "### D-", 6) == 0) {
            first_entry = content;
        } else {
            first_entry = strstr(content, "\n### D-");
            if (first_entry) first_entry++; /* skip the \n */
        }
        if (!first_entry) {
            free(content);
            fprintf(stderr, "Warning: auto-archive: no entries found\n");
            return;
        }
        /* Walk backward from first_entry over separator block "\n---\n\n" */
        const char *hend = first_entry;
        while (hend > content && *(hend - 1) == '\n') hend--;
        if (hend >= content + 3 && hend[-3] == '-' && hend[-2] == '-' && hend[-1] == '-') {
            hend -= 3;
            while (hend > content && *(hend - 1) == '\n') hend--;
            if (hend > content) hend++; /* keep one newline */
        }
        header_end = (size_t)(hend - content);
    }

    /* Build archive filename: <name>-<YYYYMMDD>-<HHMMSS>-archive.md */
    char archive_path[SCRIBE_MAX_PATH];
    {
        time_t now = time(NULL);
        struct tm tm_buf;
        struct tm *tm = gmtime_r(&now, &tm_buf);
        if (!tm) {
            free(content);
            fprintf(stderr, "Warning: auto-archive: gmtime_r failed\n");
            return;
        }
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tm);

        const char *dot = strrchr(log_path, '.');
        if (dot && strcmp(dot, ".md") == 0) {
            int prefix_len = (int)(dot - log_path);
            int n = snprintf(archive_path, sizeof(archive_path),
                             "%.*s-%s-archive.md", prefix_len, log_path, timestamp);
            if (n < 0 || (size_t)n >= sizeof(archive_path)) {
                free(content);
                fprintf(stderr, "Warning: auto-archive: archive path overflow\n");
                return;
            }
        } else {
            int n = snprintf(archive_path, sizeof(archive_path),
                             "%s-%s-archive.md", log_path, timestamp);
            if (n < 0 || (size_t)n >= sizeof(archive_path)) {
                free(content);
                fprintf(stderr, "Warning: auto-archive: archive path overflow\n");
                return;
            }
        }
    }

    /* --- Write archive file (header + first 250 entries) --- */
    char archive_tmp[SCRIBE_MAX_PATH + 8];
    {
        int n = snprintf(archive_tmp, sizeof(archive_tmp), "%s.tmp", archive_path);
        ASSERT_MSG(n >= 0 && (size_t)n < sizeof(archive_tmp),
                   "scribe_log_auto_archive: archive_tmp truncated");
    }

    int afd = open(archive_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (afd < 0) {
        free(content);
        fprintf(stderr, "Warning: auto-archive: cannot create archive tmp: %s\n",
                strerror(errno));
        return;
    }
    FILE *af = fdopen(afd, "w");
    if (!af) {
        close(afd);
        unlink(archive_tmp);
        free(content);
        fprintf(stderr, "Warning: auto-archive: fdopen failed\n");
        return;
    }

    /* Write header + archived entries */
    size_t aw = fwrite(content, 1, cleave_offset, af);
    if (aw != cleave_offset || fclose(af) != 0) {
        unlink(archive_tmp);
        free(content);
        fprintf(stderr, "Warning: auto-archive: archive write failed\n");
        return;
    }

    if (rename(archive_tmp, archive_path) != 0) {
        unlink(archive_tmp);
        free(content);
        fprintf(stderr, "Warning: auto-archive: archive rename failed: %s\n",
                strerror(errno));
        return;
    }

    /* --- Rewrite main file: header + remaining entries --- */
    /* Update "Decision count: N" in header */
    int remaining_count = decision_count - SCRIBE_ARCHIVE_CLEAVE;

    char main_tmp[SCRIBE_MAX_PATH + 8];
    {
        int n = snprintf(main_tmp, sizeof(main_tmp), "%s.tmp", log_path);
        ASSERT_MSG(n >= 0 && (size_t)n < sizeof(main_tmp),
                   "scribe_log_auto_archive: main_tmp truncated");
    }

    int mfd = open(main_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (mfd < 0) {
        free(content);
        fprintf(stderr, "Warning: auto-archive: cannot create main tmp: %s\n",
                strerror(errno));
        return;
    }
    FILE *mf = fdopen(mfd, "w");
    if (!mf) {
        close(mfd);
        unlink(main_tmp);
        free(content);
        fprintf(stderr, "Warning: auto-archive: fdopen failed for main\n");
        return;
    }

    /* Write header with updated decision count */
    int mw_err = 0;
    {
        /* Write header line by line, replacing "Decision count: ..." */
        const char *hdr = content;
        const char *hdr_end = content + header_end;
        while (hdr < hdr_end) {
            const char *line_end = memchr(hdr, '\n', (size_t)(hdr_end - hdr));
            if (!line_end) line_end = hdr_end;
            size_t line_len = (size_t)(line_end - hdr);

            if (line_len >= 16 && strncmp(hdr, "Decision count:", 15) == 0) {
                if (fprintf(mf, "Decision count: %d\n", remaining_count) < 0)
                    mw_err = 1;
            } else {
                if (fwrite(hdr, 1, line_len, mf) != line_len) mw_err = 1;
                if (fputc('\n', mf) == EOF) mw_err = 1;
            }
            hdr = line_end + 1;
        }
    }

    /* Write remaining entries (from cleave_offset to end) */
    size_t remaining_size = nread - cleave_offset;
    if (!mw_err && remaining_size > 0) {
        if (fwrite(content + cleave_offset, 1, remaining_size, mf) != remaining_size)
            mw_err = 1;
    }

    if (mw_err || fclose(mf) != 0) {
        unlink(main_tmp);
        free(content);
        fprintf(stderr, "Warning: auto-archive: main rewrite failed\n");
        return;
    }

    if (rename(main_tmp, log_path) != 0) {
        unlink(main_tmp);
        free(content);
        fprintf(stderr, "Warning: auto-archive: main rename failed: %s\n",
                strerror(errno));
        return;
    }

    free(content);

    fprintf(stderr, "nbs-scribe-log: archived %d decisions to %s (%d remaining)\n",
            SCRIBE_ARCHIVE_CLEAVE, archive_path, remaining_count);
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
    if (entry->status[0] != '\0') {
        ASSERT_MSG(strchr(entry->status, '\n') == NULL,
                   "scribe_log_append: status contains newline (injection risk)");
    }
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

    /* Append to log file.
     * SECURITY (S10): Use open()+fdopen() with explicit mode 0644 instead
     * of fopen("a") which inherits umask-dependent permissions. */
    int append_fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (append_fd < 0) {
        fprintf(stderr, "Error: cannot open log file %s: %s\n",
                log_path, strerror(errno));
        lock_release(lock_fd);
        return SCRIBE_EXIT_ERROR;
    }
    FILE *f = fdopen(append_fd, "a");
    if (!f) {
        close(append_fd);
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

    /* Auto-archive if decision count exceeds threshold.
     * Non-fatal: if archive fails, the append still succeeded.
     * Runs under the existing fcntl lock (no second lock acquired). */
    scribe_log_auto_archive(log_path);

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
