/*
 * chat_file.c — Chat file protocol implementation
 */

#include "chat_file.h"
#include "base64.h"
#include "lock.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- Internal helpers --- */

/* Safe integer parse — returns 0 on success, -1 on error */
static int safe_parse_int(const char *str, int *out) {
    ASSERT_MSG(str != NULL, "safe_parse_int: str is NULL");
    ASSERT_MSG(out != NULL, "safe_parse_int: out is NULL");
    char *endptr;
    errno = 0;
    long val = strtol(str, &endptr, 10);
    if (errno != 0 || endptr == str || (*endptr != '\0' && *endptr != '\n' && *endptr != '\r')) return -1;
    if (val < INT_MIN || val > INT_MAX) return -1;
    *out = (int)val;
    return 0;
}

static int safe_parse_int64(const char *str, int64_t *out) {
    ASSERT_MSG(str != NULL, "safe_parse_int64: str is NULL");
    ASSERT_MSG(out != NULL, "safe_parse_int64: out is NULL");
    _Static_assert(sizeof(long long) >= sizeof(int64_t),
                   "long long must be at least as wide as int64_t");
    char *endptr;
    errno = 0;
    long long val = strtoll(str, &endptr, 10);
    if (errno != 0 || endptr == str || (*endptr != '\0' && *endptr != '\n' && *endptr != '\r')) return -1;
    if (val < INT64_MIN || val > INT64_MAX) return -1;
    *out = (int64_t)val;
    return 0;
}

static void get_timestamp(char *buf, size_t buf_size) {
    ASSERT_MSG(buf != NULL, "get_timestamp: buf is NULL");
    ASSERT_MSG(buf_size > 0, "get_timestamp: buf_size must be positive, got %zu", buf_size);
    time_t now = time(NULL);
    ASSERT_MSG(now != (time_t)-1, "get_timestamp: time() failed");
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    ASSERT_MSG(tm != NULL, "get_timestamp: localtime_r() returned NULL for time %" PRId64, (int64_t)now);
    strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%S%z", tm);
}

/*
 * Compute self-consistent file-length.
 * The file-length header line is: "file-length: N\n"
 * where N is the total file size INCLUDING the line containing N.
 * This is self-referential: we must solve for N.
 */
static int64_t compute_file_length(const char *content_without_length) {
    ASSERT_MSG(content_without_length != NULL,
               "compute_file_length: content_without_length is NULL");
    /* Write content without the file-length line, measure it */
    int64_t base_size = (int64_t)strlen(content_without_length); /* content already ends with \n */

    /* The line we will insert is "file-length: N\n" = 14 + digits(N) chars */
    /* Try with current digit count */
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%" PRId64, base_size);
    int64_t digits = (int64_t)strlen(size_str);

    int64_t candidate = base_size + 14 + digits;

    /* Check if adding the line changed the digit count */
    snprintf(size_str, sizeof(size_str), "%" PRId64, candidate);
    if ((int64_t)strlen(size_str) != digits) {
        candidate = base_size + 14 + (int64_t)strlen(size_str);
    }

    /* Postcondition: self-referential consistency check.
     * "file-length: N\n" = 14 + digits_of(N) chars.
     * candidate must equal base_size + 14 + digits_of(candidate). */
    snprintf(size_str, sizeof(size_str), "%" PRId64, candidate);
    int64_t final_digits = (int64_t)strlen(size_str);
    ASSERT_MSG(candidate == base_size + 14 + final_digits,
               "compute_file_length: self-referential check failed: "
               "candidate=%" PRId64 " != base_size=%" PRId64 " + 14 + digits=%" PRId64,
               candidate, base_size, final_digits);
    ASSERT_MSG(candidate > 0,
               "compute_file_length: result must be positive, got %" PRId64, candidate);

    return candidate;
}

static int parse_participants(const char *line, participant_t *parts, int max_parts) {
    ASSERT_MSG(line != NULL, "parse_participants: line is NULL");
    ASSERT_MSG(parts != NULL, "parse_participants: parts is NULL");
    ASSERT_MSG(max_parts > 0, "parse_participants: max_parts must be positive");
    int count = 0;
    const char *p = line;

    while (*p && count < max_parts) {
        /* Skip whitespace and commas */
        while (*p == ' ' || *p == ',') p++;
        if (*p == '\0' || *p == '\n') break;

        /* Read handle */
        const char *start = p;
        while (*p && *p != '(' && *p != ',' && *p != '\n') p++;

        size_t handle_len = p - start;
        if (handle_len == 0 || handle_len >= MAX_HANDLE_LEN) break;

        strncpy(parts[count].handle, start, handle_len);
        parts[count].handle[handle_len] = '\0';

        /* Read count if present */
        parts[count].count = 0;
        if (*p == '(') {
            p++; /* skip ( */
            /* Extract numeric substring up to ')' for safe parsing */
            const char *num_start = p;
            while (*p && *p != ')') p++;
            size_t num_len = p - num_start;
            if (num_len > 0 && num_len < 16) {
                char num_buf[16];
                memcpy(num_buf, num_start, num_len);
                num_buf[num_len] = '\0';
                int parsed_count;
                if (safe_parse_int(num_buf, &parsed_count) == 0) {
                    parts[count].count = parsed_count;
                }
            }
            if (*p == ')') p++;
        }

        count++;
    }

    /* Postcondition: count is within bounds */
    ASSERT_MSG(count >= 0 && count <= max_parts,
               "parse_participants: count %d out of bounds [0, %d]",
               count, max_parts);

    return count;
}

static void format_participants(const participant_t *parts, int count,
                                 char *buf, size_t buf_size) {
    ASSERT_MSG(buf != NULL && buf_size > 0,
               "format_participants: invalid output buffer");
    buf[0] = '\0';
    size_t offset = 0;
    for (int i = 0; i < count; i++) {
        int written;
        if (i > 0) {
            written = snprintf(buf + offset, buf_size - offset, ", ");
            if (written > 0 && (size_t)written < buf_size - offset) {
                offset += (size_t)written;
            } else if (written > 0) {
                fprintf(stderr, "warning: format_participants: separator truncated at "
                        "participant %d (offset %zu, buf_size %zu)\n", i, offset, buf_size);
                offset = buf_size - 1;
                break;  /* Stop: further writes would also truncate */
            }
        }
        written = snprintf(buf + offset, buf_size - offset,
                           "%s(%d)", parts[i].handle, parts[i].count);
        if (written > 0 && (size_t)written < buf_size - offset) {
            offset += (size_t)written;
        } else if (written > 0) {
            fprintf(stderr, "warning: format_participants: entry truncated at "
                    "participant %d '%s' (offset %zu, buf_size %zu)\n",
                    i, parts[i].handle, offset, buf_size);
            offset = buf_size - 1;
            break;  /* Stop: further writes would also truncate */
        }
    }
    /* Postcondition: buffer is null-terminated within bounds */
    ASSERT_MSG(offset < buf_size,
               "format_participants: output truncated beyond buffer (offset %zu >= buf_size %zu)",
               offset, buf_size);
}

static int update_participants(participant_t *parts, int count,
                                const char *handle, int max_parts) {
    ASSERT_MSG(parts != NULL, "update_participants: parts is NULL");
    ASSERT_MSG(handle != NULL, "update_participants: handle is NULL");
    ASSERT_MSG(max_parts > 0, "update_participants: max_parts must be positive, got %d", max_parts);
    /* Find existing participant */
    for (int i = 0; i < count; i++) {
        if (strcmp(parts[i].handle, handle) == 0) {
            parts[i].count++;
            return count;
        }
    }

    /* Add new participant */
    if (count >= max_parts) return count;
    strncpy(parts[count].handle, handle, MAX_HANDLE_LEN - 1);
    parts[count].handle[MAX_HANDLE_LEN - 1] = '\0';
    /* Note: handle is truncated to MAX_HANDLE_LEN-1 if too long */
    parts[count].count = 1;
    return count + 1;
}

/* --- Auto-archive --- */

/* Forward declarations for helpers defined later in this file */
static void cursor_path(const char *chat_path, char *out, size_t out_sz);

/*
 * Archive threshold and cleave size.
 * When a chat file exceeds ARCHIVE_THRESHOLD messages after a send,
 * the first ARCHIVE_CLEAVE messages are moved to an archive file.
 */
#define ARCHIVE_THRESHOLD 2000
#define ARCHIVE_CLEAVE    1000

/*
 * chat_auto_archive — Cleave old messages into an archive file.
 *
 * Called from chat_send after the main write succeeds, while the lock
 * is still held. The caller provides the full set of encoded message
 * lines (existing + new) and their count.
 *
 * Preconditions:
 *   - Lock is held by caller
 *   - total_count > ARCHIVE_THRESHOLD
 *   - all_lines contains total_count base64-encoded message strings
 *   - path is the chat file path (absolute)
 *   - state contains current header fields (participants, last_writer, etc.)
 *
 * Postconditions:
 *   - On success (returns 0): archive file created, main file rewritten
 *     with remaining messages, cursor file adjusted
 *   - On failure (returns -1): main file is unchanged (the pre-archive
 *     version was already atomically written by chat_send)
 *
 * The archive file is named: <basename>-<date>-<time>-archive.chat
 * placed in the same directory as the chat file.
 */
static int chat_auto_archive(const char *path, char **all_lines,
                              int total_count, const chat_state_t *state) {
    ASSERT_MSG(path != NULL, "chat_auto_archive: path is NULL");
    ASSERT_MSG(all_lines != NULL, "chat_auto_archive: all_lines is NULL");
    ASSERT_MSG(total_count > ARCHIVE_THRESHOLD,
               "chat_auto_archive: total_count %d <= threshold %d",
               total_count, ARCHIVE_THRESHOLD);

    int archive_count = ARCHIVE_CLEAVE;
    int remaining_count = total_count - archive_count;

    ASSERT_MSG(remaining_count > 0,
               "chat_auto_archive: remaining_count %d must be positive",
               remaining_count);

    /* Build archive filename: <dir>/<name>-<date>-<time>-archive.chat */
    char archive_path[MAX_PATH_LEN];
    char timestamp[32];
    {
        time_t now = time(NULL);
        struct tm tm_buf;
        struct tm *tm = localtime_r(&now, &tm_buf);
        if (!tm) {
            fprintf(stderr, "warning: chat_auto_archive: localtime_r failed\n");
            return -1;
        }
        strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tm);
    }

    /* Find the last dot in the basename to insert before .chat extension */
    const char *dot = strrchr(path, '.');
    if (dot && strcmp(dot, ".chat") == 0) {
        int prefix_len = (int)(dot - path);
        int n = snprintf(archive_path, sizeof(archive_path),
                         "%.*s-%s-archive.chat", prefix_len, path, timestamp);
        if (n < 0 || (size_t)n >= sizeof(archive_path)) {
            fprintf(stderr, "warning: chat_auto_archive: archive path overflow\n");
            return -1;
        }
    } else {
        int n = snprintf(archive_path, sizeof(archive_path),
                         "%s-%s-archive.chat", path, timestamp);
        if (n < 0 || (size_t)n >= sizeof(archive_path)) {
            fprintf(stderr, "warning: chat_auto_archive: archive path overflow\n");
            return -1;
        }
    }

    /* --- Write archive file --- */
    char archive_tmp[MAX_PATH_LEN + 8];
    int n_archive_tmp = snprintf(archive_tmp, sizeof(archive_tmp), "%s.tmp", archive_path);
    ASSERT_MSG(n_archive_tmp >= 0 && (size_t)n_archive_tmp < sizeof(archive_tmp),
               "chat_auto_archive: archive_tmp truncated for %s", archive_path);

    /* Build archive header — recount participants from archived messages */
    char archive_ts[64];
    get_timestamp(archive_ts, sizeof(archive_ts));

    /* Build header without file-length for archive */
    char archive_header[4096];
    int ah_len = snprintf(archive_header, sizeof(archive_header),
        "=== nbs-chat ===\n"
        "last-writer: system\n"
        "last-write: %s\n"
        "participants: (archived)\n"
        "---\n", archive_ts);
    if (ah_len < 0 || (size_t)ah_len >= sizeof(archive_header)) {
        fprintf(stderr, "warning: chat_auto_archive: header overflow\n");
        return -1;
    }

    /* Compute archive content size for file-length */
    size_t archive_content_size = (size_t)ah_len;
    for (int i = 0; i < archive_count; i++) {
        archive_content_size += strlen(all_lines[i]) + 1;
    }
    ASSERT_MSG(archive_content_size < SIZE_MAX / 2,
               "chat_auto_archive: archive content size overflow: %zu", archive_content_size);
    char *archive_content = malloc(archive_content_size + 1);
    if (!archive_content) return -1;

    size_t aoff = 0;
    memcpy(archive_content + aoff, archive_header, (size_t)ah_len);
    aoff += (size_t)ah_len;
    for (int i = 0; i < archive_count; i++) {
        size_t ll = strlen(all_lines[i]);
        memcpy(archive_content + aoff, all_lines[i], ll);
        aoff += ll;
        archive_content[aoff++] = '\n';
    }
    archive_content[aoff] = '\0';

    /* Postcondition: all content was copied exactly */
    ASSERT_MSG(aoff == archive_content_size,
               "chat_auto_archive: content copy mismatch: "
               "aoff=%zu != archive_content_size=%zu",
               aoff, archive_content_size);

    int64_t archive_file_len = compute_file_length(archive_content);

    int afd = open(archive_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (afd < 0) {
        free(archive_content);
        fprintf(stderr, "warning: chat_auto_archive: open failed: %s\n",
                strerror(errno));
        return -1;
    }
    FILE *af = fdopen(afd, "w");
    if (!af) {
        close(afd);
        unlink(archive_tmp);
        free(archive_content);
        return -1;
    }

    int aw_err = 0;
    if (fprintf(af, "=== nbs-chat ===\n") < 0) aw_err = 1;
    if (!aw_err && fprintf(af, "last-writer: system\n") < 0) aw_err = 1;
    if (!aw_err && fprintf(af, "last-write: %s\n", archive_ts) < 0) aw_err = 1;
    if (!aw_err && fprintf(af, "file-length: %" PRId64 "\n", archive_file_len) < 0) aw_err = 1;
    if (!aw_err && fprintf(af, "participants: (archived)\n") < 0) aw_err = 1;
    if (!aw_err && fprintf(af, "---\n") < 0) aw_err = 1;
    for (int i = 0; i < archive_count && !aw_err; i++) {
        if (fprintf(af, "%s\n", all_lines[i]) < 0) aw_err = 1;
    }
    free(archive_content);

    if (aw_err || fclose(af) != 0) {
        unlink(archive_tmp);
        fprintf(stderr, "warning: chat_auto_archive: archive write failed\n");
        return -1;
    }

    if (rename(archive_tmp, archive_path) != 0) {
        unlink(archive_tmp);
        fprintf(stderr, "warning: chat_auto_archive: archive rename failed: %s\n",
                strerror(errno));
        return -1;
    }

    /* --- Rewrite main file with remaining messages --- */
    /* Reuse the existing header fields from state (participants stay cumulative) */
    char parts_str[4096];
    format_participants(state->participants, state->participant_count,
                        parts_str, sizeof(parts_str));

    /* Build content without file-length for the trimmed file */
    char main_header[4096];
    int mh_len = snprintf(main_header, sizeof(main_header),
        "=== nbs-chat ===\n"
        "last-writer: %s\n"
        "last-write: %s\n"
        "participants: %s\n"
        "---\n", state->last_writer, state->last_write, parts_str);
    if (mh_len < 0 || (size_t)mh_len >= sizeof(main_header)) {
        fprintf(stderr, "warning: chat_auto_archive: main header overflow\n");
        return -1;
    }

    size_t main_content_size = (size_t)mh_len;
    for (int i = archive_count; i < total_count; i++) {
        main_content_size += strlen(all_lines[i]) + 1;
    }
    ASSERT_MSG(main_content_size < SIZE_MAX / 2,
               "chat_auto_archive: main content size overflow: %zu", main_content_size);
    char *main_content = malloc(main_content_size + 1);
    if (!main_content) return -1;

    size_t moff = 0;
    memcpy(main_content + moff, main_header, (size_t)mh_len);
    moff += (size_t)mh_len;
    for (int i = archive_count; i < total_count; i++) {
        size_t ll = strlen(all_lines[i]);
        memcpy(main_content + moff, all_lines[i], ll);
        moff += ll;
        main_content[moff++] = '\n';
    }
    main_content[moff] = '\0';

    int64_t main_file_len = compute_file_length(main_content);
    free(main_content);

    char main_tmp[MAX_PATH_LEN + 8];
    int n_main_tmp = snprintf(main_tmp, sizeof(main_tmp), "%s.tmp", path);
    ASSERT_MSG(n_main_tmp >= 0 && (size_t)n_main_tmp < sizeof(main_tmp),
               "chat_auto_archive: main_tmp truncated for %s", path);

    int mfd = open(main_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (mfd < 0) {
        fprintf(stderr, "warning: chat_auto_archive: main rewrite open failed\n");
        return -1;
    }
    FILE *mf = fdopen(mfd, "w");
    if (!mf) {
        close(mfd);
        unlink(main_tmp);
        return -1;
    }

    int mw_err = 0;
    if (fprintf(mf, "=== nbs-chat ===\n") < 0) mw_err = 1;
    if (!mw_err && fprintf(mf, "last-writer: %s\n", state->last_writer) < 0) mw_err = 1;
    if (!mw_err && fprintf(mf, "last-write: %s\n", state->last_write) < 0) mw_err = 1;
    if (!mw_err && fprintf(mf, "file-length: %" PRId64 "\n", main_file_len) < 0) mw_err = 1;
    if (!mw_err && fprintf(mf, "participants: %s\n", parts_str) < 0) mw_err = 1;
    if (!mw_err && fprintf(mf, "---\n") < 0) mw_err = 1;
    for (int i = archive_count; i < total_count && !mw_err; i++) {
        if (fprintf(mf, "%s\n", all_lines[i]) < 0) mw_err = 1;
    }

    if (mw_err || fclose(mf) != 0) {
        unlink(main_tmp);
        fprintf(stderr, "warning: chat_auto_archive: main rewrite failed\n");
        return -1;
    }

    if (rename(main_tmp, path) != 0) {
        unlink(main_tmp);
        fprintf(stderr, "warning: chat_auto_archive: main rename failed: %s\n",
                strerror(errno));
        return -1;
    }

    /* --- Adjust cursors --- */
    char cpath[MAX_PATH_LEN];
    cursor_path(path, cpath, sizeof(cpath));

    FILE *cf = fopen(cpath, "r");
    if (cf) {
        char handles[MAX_PARTICIPANTS][MAX_HANDLE_LEN];
        int indices[MAX_PARTICIPANTS];
        int ccount = 0;
        char line[256];

        while (fgets(line, sizeof(line), cf) && ccount < MAX_PARTICIPANTS) {
            if (line[0] == '#' || line[0] == '\n') continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            size_t klen = (size_t)(eq - line);
            if (klen >= MAX_HANDLE_LEN) continue;
            memcpy(handles[ccount], line, klen);
            handles[ccount][klen] = '\0';
            if (safe_parse_int(eq + 1, &indices[ccount]) != 0) {
                indices[ccount] = 0;
            }
            /* Decrement by archive_count, clamp to 0 */
            indices[ccount] -= archive_count;
            if (indices[ccount] < 0) indices[ccount] = 0;
            ccount++;
        }
        fclose(cf);

        /* Write adjusted cursors atomically */
        char cursor_tmp[MAX_PATH_LEN + 8];
        int n_cursor_tmp = snprintf(cursor_tmp, sizeof(cursor_tmp), "%s.tmp", cpath);
        ASSERT_MSG(n_cursor_tmp >= 0 && (size_t)n_cursor_tmp < sizeof(cursor_tmp),
                   "chat_auto_archive: cursor_tmp truncated for %s", cpath);
        int cfd = open(cursor_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (cfd >= 0) {
            FILE *cwf = fdopen(cfd, "w");
            if (cwf) {
                if (fprintf(cwf, "# Read cursors — last-read message index per handle\n") < 0) {
                    fclose(cwf);
                    unlink(cursor_tmp);
                    return -1;
                }
                for (int i = 0; i < ccount; i++) {
                    if (fprintf(cwf, "%s=%d\n", handles[i], indices[i]) < 0) {
                        fclose(cwf);
                        unlink(cursor_tmp);
                        return -1;
                    }
                }
                if (fclose(cwf) == 0) {
                    if (rename(cursor_tmp, cpath) != 0) {
                        fprintf(stderr, "warning: chat_auto_archive: cursor rename failed: %s\n",
                                strerror(errno));
                        unlink(cursor_tmp);
                    }
                } else {
                    unlink(cursor_tmp);
                }
            } else {
                close(cfd);
            }
        }
    }

    fprintf(stderr, "nbs-chat: archived %d messages to %s (%d remaining)\n",
            archive_count, archive_path, remaining_count);

    return 0;
}

/* --- Invariant validation --- */

int chat_state_check_invariants(const chat_state_t *state) {
    ASSERT_MSG(state != NULL, "chat_state_check_invariants: state is NULL");

    if (state->message_count < 0 || state->message_count > MAX_MESSAGES) {
        fprintf(stderr, "invariant violation: message_count %d out of [0, %d]\n",
                state->message_count, MAX_MESSAGES);
        return 0;
    }
    if (state->participant_count < 0 || state->participant_count > MAX_PARTICIPANTS) {
        fprintf(stderr, "invariant violation: participant_count %d out of [0, %d]\n",
                state->participant_count, MAX_PARTICIPANTS);
        return 0;
    }
    if (state->message_count > 0 && state->messages == NULL) {
        fprintf(stderr, "invariant violation: messages is NULL but message_count is %d\n",
                state->message_count);
        return 0;
    }
    /* Check participant counts are non-negative */
    for (int i = 0; i < state->participant_count; i++) {
        if (state->participants[i].count < 0) {
            fprintf(stderr, "invariant violation: participant %d '%s' has negative count %d\n",
                    i, state->participants[i].handle, state->participants[i].count);
            return 0;
        }
    }
    return 1;
}

/* --- Public API --- */

int chat_create(const char *path) {
    ASSERT_MSG(path != NULL, "chat_create: path is NULL");

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    /* Build content without file-length line */
    char content[1024];
    int len = snprintf(content, sizeof(content),
        "=== nbs-chat ===\n"
        "last-writer: system\n"
        "last-write: %s\n"
        "participants: \n"
        "---\n",
        timestamp);

    if (len < 0 || (size_t)len >= sizeof(content)) return -2;

    int64_t file_len = compute_file_length(content);

    /* Atomic create-or-fail: O_EXCL ensures no TOCTOU race.
     * If the file already exists, open() fails with EEXIST. */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST) return -1; /* Already exists */
        return -2;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return -2; }

    int write_err = 0;
    if (fprintf(f, "=== nbs-chat ===\n") < 0) write_err = 1;
    if (!write_err && fprintf(f, "last-writer: system\n") < 0) write_err = 1;
    if (!write_err && fprintf(f, "last-write: %s\n", timestamp) < 0) write_err = 1;
    if (!write_err && fprintf(f, "file-length: %" PRId64 "\n", file_len) < 0) write_err = 1;
    if (!write_err && fprintf(f, "participants: \n") < 0) write_err = 1;
    if (!write_err && fprintf(f, "---\n") < 0) write_err = 1;
    if (write_err) {
        fclose(f);
        unlink(path);
        return -2;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "warning: chat_create: fclose failed: %s\n", strerror(errno));
        return -2;
    }

    /* Postcondition: verify file-length matches actual size */
    struct stat st;
    int stat_rc = stat(path, &st);
    ASSERT_MSG(stat_rc == 0,
               "chat_create: stat failed after write: %s", strerror(errno));
    ASSERT_MSG((int64_t)st.st_size == file_len,
               "chat_create postcondition: file-length header %" PRId64 " != actual size %" PRId64,
               file_len, (int64_t)st.st_size);

    return 0;
}

int chat_read(const char *path, chat_state_t *state) {
    ASSERT_MSG(path != NULL, "chat_read: path is NULL");
    ASSERT_MSG(state != NULL, "chat_read: state is NULL");

    memset(state, 0, sizeof(*state));

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[MAX_MESSAGE_LEN];
    int in_header = 0;
    int past_header = 0;
    int line_number = 0;

    /* Temporary message storage */
    _Static_assert(MAX_MESSAGES <= SIZE_MAX / sizeof(chat_message_t),
                   "MAX_MESSAGES * sizeof(chat_message_t) would overflow size_t");
    state->messages = malloc(sizeof(chat_message_t) * MAX_MESSAGES);
    if (!state->messages) {
        fclose(f);
        return -1;
    }
    state->message_count = 0;
    state->skipped_count = 0;

    while (fgets(line, sizeof(line), f)) {
        line_number++;
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (strcmp(line, "=== nbs-chat ===") == 0) {
            in_header = 1;
            continue;
        }

        if (in_header && strcmp(line, "---") == 0) {
            in_header = 0;
            past_header = 1;
            continue;
        }

        if (in_header) {
            /* Parse header fields */
            if (strncmp(line, "last-writer: ", 13) == 0) {
                snprintf(state->last_writer, MAX_HANDLE_LEN, "%.*s",
                         (int)(MAX_HANDLE_LEN - 1), line + 13);
            } else if (strncmp(line, "last-write: ", 12) == 0) {
                snprintf(state->last_write, sizeof(state->last_write), "%.*s",
                         (int)(sizeof(state->last_write) - 1), line + 12);
            } else if (strncmp(line, "file-length: ", 13) == 0) {
                if (safe_parse_int64(line + 13, &state->file_length) != 0) {
                    fprintf(stderr, "warning: chat_read: invalid file-length value: %s\n", line + 13);
                }
            } else if (strncmp(line, "participants: ", 14) == 0) {
                state->participant_count = parse_participants(
                    line + 14, state->participants, MAX_PARTICIPANTS);
            }
            continue;
        }

        if (past_header && len > 0 && state->message_count < MAX_MESSAGES) {
            /* Validate base64 padding before decode — corrupt lines with
             * length not a multiple of 4 would trigger the precondition
             * assert in base64_decoded_size and abort the process. Skip
             * corrupt lines gracefully instead. */
            if (len % 4 != 0) {
                fprintf(stderr, "warning: chat_read: corrupt base64 at line %d "
                        "(len=%zu not multiple of 4, first 20 chars: '%.20s'), skipping\n",
                        line_number, len, line);
                state->skipped_count++;
                continue;
            }

            /* Decode base64 message */
            size_t decoded_max = base64_decoded_size(len);
            unsigned char *decoded = malloc(decoded_max + 1);
            if (!decoded) {
                fprintf(stderr, "warning: chat_read: malloc failed for message at line %d, skipping\n", line_number);
                state->skipped_count++;
                continue;
            }

            int decoded_len = base64_decode(line, len, decoded, decoded_max);
            if (decoded_len < 0) {
                fprintf(stderr, "warning: chat_read: base64 decode failed at line %d "
                        "(len=%zu, first 20 chars: '%.20s'), skipping\n",
                        line_number, len, line);
                free(decoded);
                state->skipped_count++;
                continue;
            }
            decoded[decoded_len] = '\0';

            /* Parse wire formats:
             *   handle|EPOCH|SIG: content  (signed, timestamped)
             *   handle|EPOCH: content      (unsigned, timestamped)
             *   handle: content            (legacy, unsigned)
             */
            char *colon = strstr((char *)decoded, ": ");
            if (colon) {
                size_t prefix_len = colon - (char *)decoded;
                /* Check for pipe separator(s) */
                char *pipe1 = memchr((char *)decoded, '|', prefix_len);
                size_t handle_len;
                time_t msg_timestamp = 0;
                if (pipe1) {
                    handle_len = pipe1 - (char *)decoded;
                    /* Look for second pipe between pipe1+1 and colon */
                    size_t after_pipe1 = colon - (pipe1 + 1);
                    char *pipe2 = memchr(pipe1 + 1, '|', after_pipe1);

                    if (pipe2) {
                        /* Legacy signed format: handle|EPOCH|SIG: content
                         * Auth system removed — just extract the timestamp,
                         * ignore the SIG field. */
                        size_t epoch_len = pipe2 - (pipe1 + 1);
                        if (epoch_len > 0 && epoch_len < 20) {
                            char epoch_buf[20];
                            memcpy(epoch_buf, pipe1 + 1, epoch_len);
                            epoch_buf[epoch_len] = '\0';
                            int64_t parsed_epoch;
                            if (safe_parse_int64(epoch_buf, &parsed_epoch) == 0 && parsed_epoch > 0) {
                                msg_timestamp = (time_t)parsed_epoch;
                            }
                        }
                    } else {
                        /* Unsigned timestamped: handle|EPOCH: content */
                        size_t epoch_len = colon - (pipe1 + 1);
                        if (epoch_len > 0 && epoch_len < 20) {
                            char epoch_buf[20];
                            memcpy(epoch_buf, pipe1 + 1, epoch_len);
                            epoch_buf[epoch_len] = '\0';
                            int64_t parsed_epoch;
                            if (safe_parse_int64(epoch_buf, &parsed_epoch) == 0 && parsed_epoch > 0) {
                                msg_timestamp = (time_t)parsed_epoch;
                            }
                        }
                    }
                } else {
                    /* Legacy format: handle: content */
                    handle_len = prefix_len;
                }

                if (handle_len < MAX_HANDLE_LEN && handle_len > 0) {
                    /* Precondition: colon+2 must be within the decoded buffer */
                    ASSERT_MSG(colon + 2 >= (char *)decoded &&
                               (size_t)(colon + 2 - (char *)decoded) <= (size_t)decoded_len,
                               "chat_read: colon+2 offset %td out of bounds [0, %d] "
                               "for message at line %d",
                               colon + 2 - (char *)decoded, decoded_len, line_number);
                    chat_message_t *msg = &state->messages[state->message_count];
                    strncpy(msg->handle, (char *)decoded, handle_len);
                    msg->handle[handle_len] = '\0';
                    msg->content = strdup(colon + 2);
                    if (!msg->content) {
                        fprintf(stderr, "warning: chat_read: strdup failed at line %d\n", line_number);
                        free(decoded);
                        state->skipped_count++;
                        continue;
                    }
                    msg->content_len = decoded_len - (colon + 2 - (char *)decoded);
                    msg->timestamp = msg_timestamp;
                    /* Invariant: content_len == strlen(content) — no embedded NULs */
                    ASSERT_MSG(msg->content_len == strlen(msg->content),
                               "chat_read: content_len %zu != strlen(content) %zu for message %d"
                               " — embedded NUL detected",
                               msg->content_len, strlen(msg->content), state->message_count);
                    state->message_count++;
                }
            }

            free(decoded);
        }
    }

    /* Invariant: message_count must be within bounds */
    ASSERT_MSG(state->message_count >= 0 && state->message_count <= MAX_MESSAGES,
               "chat_read: message_count %d out of bounds [0, %d]",
               state->message_count, MAX_MESSAGES);

    /* Check for I/O errors during read */
    if (ferror(f)) {
        fprintf(stderr, "warning: chat_read: I/O error reading %s\n", path);
        fclose(f);
        chat_state_free(state);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "warning: chat_read: fclose failed for %s: %s\n", path, strerror(errno));
        chat_state_free(state);
        return -1;
    }
    return 0;
}

int chat_send(const char *path, const char *handle, const char *message) {
    ASSERT_MSG(path != NULL, "chat_send: path is NULL");
    ASSERT_MSG(handle != NULL, "chat_send: handle is NULL");
    ASSERT_MSG(handle[0] != '\0', "chat_send: handle is empty");
    ASSERT_MSG(strlen(handle) < MAX_HANDLE_LEN,
           "chat_send: handle too long: %zu >= %d", strlen(handle), MAX_HANDLE_LEN);
    ASSERT_MSG(message != NULL, "chat_send: message is NULL");

    int lock_fd = chat_lock_acquire(path);
    if (lock_fd < 0) return -1;

    /* Read current state */
    chat_state_t state;
    if (chat_read(path, &state) < 0) {
        chat_lock_release(lock_fd);
        return -1;
    }

    /* Build the message line: "handle|EPOCH: message" */
    time_t now = time(NULL);
    ASSERT_MSG(now != (time_t)-1, "chat_send: time() failed");
    char epoch_str[24];
    snprintf(epoch_str, sizeof(epoch_str), "%" PRId64, (int64_t)now);

    size_t raw_len = strlen(handle) + 1 + strlen(epoch_str) + 2 + strlen(message);
    char *raw = malloc(raw_len + 1);
    if (!raw) {
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }
    snprintf(raw, raw_len + 1, "%s|%s: %s", handle, epoch_str, message);

    /* Postcondition: raw message was fully written */
    ASSERT_MSG(raw_len > 0,
               "chat_send: raw message length is zero for handle '%s'", handle);

    /* Base64 encode */
    size_t encoded_size = base64_encoded_size(raw_len);
    char *encoded = malloc(encoded_size);
    if (!encoded) {
        free(raw);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }
    base64_encode((unsigned char *)raw, raw_len, encoded, encoded_size);
    free(raw);

    /* Update participants */
    state.participant_count = update_participants(
        state.participants, state.participant_count, handle, MAX_PARTICIPANTS);

    /* Update header fields */
    snprintf(state.last_writer, MAX_HANDLE_LEN, "%s", handle);
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    snprintf(state.last_write, sizeof(state.last_write), "%s", timestamp);

    /* Build the file content without file-length line */
    /* First, calculate total size needed */
    char parts_str[4096];
    /* Worst case per participant: handle(count), = MAX_HANDLE_LEN + ~12 + 2 (separator) ~= 78 bytes */
    ASSERT_MSG((size_t)state.participant_count * (MAX_HANDLE_LEN + 14) < sizeof(parts_str),
               "chat_send: participant count %d * max entry size exceeds parts_str buffer %zu",
               state.participant_count, sizeof(parts_str));
    format_participants(state.participants, state.participant_count,
                        parts_str, sizeof(parts_str));

    /* Build header */
    char header[8192];
    int header_len = snprintf(header, sizeof(header),
        "=== nbs-chat ===\n"
        "last-writer: %s\n"
        "last-write: %s\n"
        "participants: %s\n"
        "---\n",
        state.last_writer, state.last_write, parts_str);
    ASSERT_MSG(header_len > 0 && (size_t)header_len < sizeof(header),
               "chat_send: header snprintf truncated or failed: %d (buffer %zu)",
               header_len, sizeof(header));

    /* Calculate total content size (header + existing messages + new message) */

    /* Read the raw file to get existing encoded lines */
    FILE *f = fopen(path, "r");
    if (!f) {
        free(encoded);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    /* Collect existing encoded lines */
    char **encoded_lines = NULL;
    int encoded_line_count = 0;
    char line_buf[MAX_MESSAGE_LEN];
    int past_delim = 0;
    int seen_header_marker = 0;

    while (fgets(line_buf, sizeof(line_buf), f)) {
        size_t ll = strlen(line_buf);
        while (ll > 0 && (line_buf[ll-1] == '\n' || line_buf[ll-1] == '\r'))
            line_buf[--ll] = '\0';

        if (!past_delim) {
            if (strcmp(line_buf, "=== nbs-chat ===") == 0) {
                seen_header_marker = 1;
            } else if (strcmp(line_buf, "---") == 0 && seen_header_marker) {
                past_delim = 1;
            }
            continue;
        }

        if (ll > 0) {
            ASSERT_MSG(encoded_line_count < MAX_MESSAGES,
                       "chat_send: encoded_line_count %d exceeds MAX_MESSAGES", encoded_line_count);
            char **tmp = realloc(encoded_lines,
                                     sizeof(char *) * (encoded_line_count + 1));
            if (!tmp) {
                fprintf(stderr, "warning: chat_send: realloc failed for %d encoded lines\n", encoded_line_count + 1);
                for (int j = 0; j < encoded_line_count; j++) free(encoded_lines[j]);
                free(encoded_lines);
                free(encoded);
                chat_state_free(&state);
                chat_lock_release(lock_fd);
                return -1;
            }
            encoded_lines = tmp;
            encoded_lines[encoded_line_count] = strdup(line_buf);
            if (!encoded_lines[encoded_line_count]) {
                fprintf(stderr, "warning: chat_send: strdup failed for encoded line %d\n", encoded_line_count);
                for (int j = 0; j < encoded_line_count; j++) free(encoded_lines[j]);
                free(encoded_lines);
                free(encoded);
                chat_state_free(&state);
                chat_lock_release(lock_fd);
                return -1;
            }
            encoded_line_count++;
        }
    }
    if (ferror(f)) {
        fprintf(stderr, "warning: chat_send: I/O error reading encoded lines from %s\n", path);
        fclose(f);
        for (int j = 0; j < encoded_line_count; j++) free(encoded_lines[j]);
        free(encoded_lines);
        free(encoded);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "warning: chat_send: fclose failed reading %s: %s\n", path, strerror(errno));
        for (int j = 0; j < encoded_line_count; j++) free(encoded_lines[j]);
        free(encoded_lines);
        free(encoded);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    /* Invariant: encoded_line_count must be non-negative */
    ASSERT_MSG(encoded_line_count >= 0,
               "chat_send: encoded_line_count went negative: %d", encoded_line_count);

    /* Calculate content without file-length for size computation */
    size_t content_size = header_len;
    for (int i = 0; i < encoded_line_count; i++) {
        content_size += strlen(encoded_lines[i]) + 1; /* +1 for \n */
    }
    content_size += strlen(encoded) + 1; /* new message + \n */
    ASSERT_MSG(content_size < SIZE_MAX / 2,
               "chat_send: content size overflow: %zu", content_size);

    char *content_no_fl = malloc(content_size + 1);
    if (!content_no_fl) {
        for (int i = 0; i < encoded_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        free(encoded);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    size_t offset = 0;
    memcpy(content_no_fl + offset, header, header_len);
    offset += header_len;
    for (int i = 0; i < encoded_line_count; i++) {
        size_t ll = strlen(encoded_lines[i]);
        memcpy(content_no_fl + offset, encoded_lines[i], ll);
        offset += ll;
        content_no_fl[offset++] = '\n';
    }
    size_t enc_len = strlen(encoded);
    memcpy(content_no_fl + offset, encoded, enc_len);
    offset += enc_len;
    content_no_fl[offset++] = '\n';
    content_no_fl[offset] = '\0';

    int64_t file_len = compute_file_length(content_no_fl);

    /* Atomic write: write to .tmp file, then rename over the target.
     * This prevents data loss if the process crashes mid-write —
     * the original file remains intact until rename() atomically
     * replaces it. */
    char tmp_path[MAX_PATH_LEN + 8];
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    ASSERT_MSG(tn > 0 && (size_t)tn < sizeof(tmp_path),
               "chat_send: tmp_path overflow for %s", path);

    int wfd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (wfd < 0) {
        free(content_no_fl);
        for (int i = 0; i < encoded_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        free(encoded);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }
    f = fdopen(wfd, "w");
    if (!f) {
        close(wfd);
        unlink(tmp_path);
        free(content_no_fl);
        for (int i = 0; i < encoded_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        free(encoded);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    int write_err = 0;
    if (fprintf(f, "=== nbs-chat ===\n") < 0) write_err = 1;
    if (!write_err && fprintf(f, "last-writer: %s\n", state.last_writer) < 0) write_err = 1;
    if (!write_err && fprintf(f, "last-write: %s\n", state.last_write) < 0) write_err = 1;
    if (!write_err && fprintf(f, "file-length: %" PRId64 "\n", file_len) < 0) write_err = 1;
    if (!write_err && fprintf(f, "participants: %s\n", parts_str) < 0) write_err = 1;
    if (!write_err && fprintf(f, "---\n") < 0) write_err = 1;
    for (int i = 0; i < encoded_line_count && !write_err; i++) {
        if (fprintf(f, "%s\n", encoded_lines[i]) < 0) write_err = 1;
    }
    if (!write_err && fprintf(f, "%s\n", encoded) < 0) write_err = 1;
    if (write_err) {
        fprintf(stderr, "error: chat_send: write failed for %s: %s\n",
                tmp_path, strerror(errno));
        fclose(f);
        unlink(tmp_path);
        free(content_no_fl);
        for (int i = 0; i < encoded_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        free(encoded);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "warning: chat_send: fclose failed: %s\n", strerror(errno));
        unlink(tmp_path);
        free(content_no_fl);
        for (int i = 0; i < encoded_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        free(encoded);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -2;
    }

    /* Postcondition: verify file-length matches actual .tmp size.
     * A mismatch means compute_file_length has a bug; the data
     * written is inconsistent with the header. Per engineering
     * standards: never silently continue after an invariant violation. */
    struct stat st;
    int stat_rc = stat(tmp_path, &st);
    ASSERT_MSG(stat_rc == 0,
               "chat_send: stat failed on %s after write: %s",
               tmp_path, strerror(errno));
    ASSERT_MSG((int64_t)st.st_size == file_len,
               "chat_send postcondition: file-length header %" PRId64
               " != actual size %" PRId64 " — compute_file_length has a bug",
               file_len, (int64_t)st.st_size);

    /* Atomic rename: replace the original file with the .tmp file.
     * rename() is atomic on POSIX filesystems (same filesystem). */
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "error: chat_send: rename %s -> %s failed: %s\n",
                tmp_path, path, strerror(errno));
        unlink(tmp_path);
        free(content_no_fl);
        for (int i = 0; i < encoded_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        free(encoded);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    /* Auto-archive: if total messages exceed threshold, cleave old messages.
     * This runs under the same lock as the send, ensuring atomicity.
     * Total messages = encoded_line_count (existing) + 1 (new). */
    int total_after_send = encoded_line_count + 1;
    int archived = 0;
    if (total_after_send > ARCHIVE_THRESHOLD) {
        /* Build combined array: existing lines + new encoded message */
        char **all_lines = malloc((size_t)total_after_send * sizeof(char *));
        if (!all_lines) {
            fprintf(stderr, "warning: chat_send: malloc failed for archive line array "
                    "(%d entries) — archiving skipped\n", total_after_send);
        }
        if (all_lines) {
            for (int i = 0; i < encoded_line_count; i++) {
                all_lines[i] = encoded_lines[i];
            }
            all_lines[encoded_line_count] = encoded;

            int arc_rc = chat_auto_archive(path, all_lines, total_after_send, &state);
            if (arc_rc == 0) {
                archived = 1;
            }
            free(all_lines);
        }
    }

    /* Cleanup */
    free(content_no_fl);
    for (int i = 0; i < encoded_line_count; i++) free(encoded_lines[i]);
    free(encoded_lines);
    free(encoded);
    chat_state_free(&state);
    chat_lock_release(lock_fd);

    /* Cursor-on-write: update sender's read cursor to the index of the
     * message just written. This prevents the sidecar from treating the
     * sender's own message as "unread" and avoids cursor desync after
     * restarts. The new message is at index encoded_line_count (0-based
     * count of messages that existed before the append).
     *
     * If archiving happened, cursors were already adjusted inside
     * chat_auto_archive (decremented by ARCHIVE_CLEAVE). The sender's
     * cursor must also reflect the post-archive index.
     *
     * This is called AFTER lock release so chat_cursor_write can acquire
     * the lock independently. The race window (another message arriving
     * between send and cursor update) is benign: the cursor will be at
     * our message or later, which is correct either way. */
    int cursor_index = encoded_line_count;
    if (archived) {
        cursor_index = encoded_line_count - ARCHIVE_CLEAVE;
        if (cursor_index < 0) cursor_index = 0;
    }
    int cw_rc = chat_cursor_write(path, handle, cursor_index);
    if (cw_rc < 0) {
        fprintf(stderr, "warning: chat_send: cursor-on-write failed for handle '%s'\n", handle);
        /* Non-fatal: the send succeeded, cursor update is best-effort */
    }

    return 0;
}

int chat_truncate(const char *path, int keep_count) {
    ASSERT_MSG(path != NULL, "chat_truncate: path is NULL");
    ASSERT_MSG(keep_count >= 0, "chat_truncate: keep_count %d is negative", keep_count);

    int lock_fd = chat_lock_acquire(path);
    if (lock_fd < 0) return -1;

    /* Read current state for decoded message data (handles, timestamps) */
    chat_state_t state;
    if (chat_read(path, &state) < 0) {
        chat_lock_release(lock_fd);
        return -1;
    }

    /* No-op if keep_count >= message_count */
    if (keep_count >= state.message_count) {
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return 0;
    }

    /* Read raw base64 lines from file (same pattern as chat_send) */
    FILE *f = fopen(path, "r");
    if (!f) {
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    char **encoded_lines = NULL;
    int stored_line_count = 0;  /* Lines actually allocated in encoded_lines */
    int total_line_count = 0;   /* Total message lines in file (including uncollected) */
    char line_buf[MAX_MESSAGE_LEN];
    int past_delim = 0;
    int seen_header_marker = 0;

    while (fgets(line_buf, sizeof(line_buf), f)) {
        size_t ll = strlen(line_buf);
        while (ll > 0 && (line_buf[ll-1] == '\n' || line_buf[ll-1] == '\r'))
            line_buf[--ll] = '\0';

        if (!past_delim) {
            if (strcmp(line_buf, "=== nbs-chat ===") == 0) {
                seen_header_marker = 1;
            } else if (strcmp(line_buf, "---") == 0 && seen_header_marker) {
                past_delim = 1;
            }
            continue;
        }

        if (ll > 0) {
            total_line_count++;
            /* Only collect lines up to keep_count */
            if (stored_line_count >= keep_count) {
                continue;  /* Count but don't store */
            }
            char **tmp = realloc(encoded_lines,
                                 sizeof(char *) * (stored_line_count + 1));
            if (!tmp) {
                for (int j = 0; j < stored_line_count; j++) free(encoded_lines[j]);
                free(encoded_lines);
                fclose(f);
                chat_state_free(&state);
                chat_lock_release(lock_fd);
                return -1;
            }
            encoded_lines = tmp;
            encoded_lines[stored_line_count] = strdup(line_buf);
            if (!encoded_lines[stored_line_count]) {
                for (int j = 0; j < stored_line_count; j++) free(encoded_lines[j]);
                free(encoded_lines);
                fclose(f);
                chat_state_free(&state);
                chat_lock_release(lock_fd);
                return -1;
            }
            stored_line_count++;
        }
    }
    if (ferror(f)) {
        fprintf(stderr, "warning: chat_truncate: I/O error reading %s\n", path);
        fclose(f);
        for (int j = 0; j < stored_line_count; j++) free(encoded_lines[j]);
        free(encoded_lines);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "warning: chat_truncate: fclose failed reading %s: %s\n", path, strerror(errno));
        for (int j = 0; j < stored_line_count; j++) free(encoded_lines[j]);
        free(encoded_lines);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    /* Postcondition: stored_line_count <= keep_count and <= total_line_count */
    ASSERT_MSG(stored_line_count <= keep_count,
               "chat_truncate: stored_line_count %d > keep_count %d",
               stored_line_count, keep_count);
    ASSERT_MSG(stored_line_count <= total_line_count,
               "chat_truncate: stored_line_count %d > total_line_count %d",
               stored_line_count, total_line_count);

    /* Recompute participants from kept messages */
    participant_t parts[MAX_PARTICIPANTS];
    int part_count = 0;
    for (int i = 0; i < keep_count && i < state.message_count; i++) {
        part_count = update_participants(parts, part_count,
                                         state.messages[i].handle, MAX_PARTICIPANTS);
    }

    /* Set last-writer and last-write from last kept message */
    char last_writer[MAX_HANDLE_LEN];
    char last_write[64];
    if (keep_count > 0 && keep_count <= state.message_count) {
        snprintf(last_writer, sizeof(last_writer), "%s",
                 state.messages[keep_count - 1].handle);
        time_t last_ts = state.messages[keep_count - 1].timestamp;
        if (last_ts > 0) {
            struct tm tm_buf;
            struct tm *tm = localtime_r(&last_ts, &tm_buf);
            if (tm) {
                strftime(last_write, sizeof(last_write),
                         "%Y-%m-%dT%H:%M:%S%z", tm);
            } else {
                get_timestamp(last_write, sizeof(last_write));
            }
        } else {
            get_timestamp(last_write, sizeof(last_write));
        }
    } else {
        /* Truncating to 0 messages */
        last_writer[0] = '\0';
        get_timestamp(last_write, sizeof(last_write));
    }

    /* Format participants string */
    char parts_str[4096];
    format_participants(parts, part_count, parts_str, sizeof(parts_str));

    /* Build header (without file-length) */
    char header[8192];
    int header_len = snprintf(header, sizeof(header),
        "=== nbs-chat ===\n"
        "last-writer: %s\n"
        "last-write: %s\n"
        "participants: %s\n"
        "---\n",
        last_writer, last_write, parts_str);
    ASSERT_MSG(header_len > 0 && (size_t)header_len < sizeof(header),
               "chat_truncate: header snprintf overflow");

    /* Build content without file-length for size computation */
    size_t content_size = header_len;
    for (int i = 0; i < stored_line_count; i++) {
        content_size += strlen(encoded_lines[i]) + 1;
    }

    char *content_no_fl = malloc(content_size + 1);
    if (!content_no_fl) {
        for (int i = 0; i < stored_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    size_t offset = 0;
    memcpy(content_no_fl + offset, header, header_len);
    offset += header_len;
    for (int i = 0; i < stored_line_count; i++) {
        size_t ll = strlen(encoded_lines[i]);
        memcpy(content_no_fl + offset, encoded_lines[i], ll);
        offset += ll;
        content_no_fl[offset++] = '\n';
    }
    content_no_fl[offset] = '\0';

    int64_t file_len = compute_file_length(content_no_fl);

    /* Atomic write: .tmp then rename */
    char tmp_path[MAX_PATH_LEN + 8];
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    ASSERT_MSG(tn > 0 && (size_t)tn < sizeof(tmp_path),
               "chat_truncate: tmp_path overflow");

    int wfd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (wfd < 0) {
        free(content_no_fl);
        for (int i = 0; i < stored_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }
    f = fdopen(wfd, "w");
    if (!f) {
        close(wfd);
        unlink(tmp_path);
        free(content_no_fl);
        for (int i = 0; i < stored_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    int write_err = 0;
    if (fprintf(f, "=== nbs-chat ===\n") < 0) write_err = 1;
    if (!write_err && fprintf(f, "last-writer: %s\n", last_writer) < 0) write_err = 1;
    if (!write_err && fprintf(f, "last-write: %s\n", last_write) < 0) write_err = 1;
    if (!write_err && fprintf(f, "file-length: %" PRId64 "\n", file_len) < 0) write_err = 1;
    if (!write_err && fprintf(f, "participants: %s\n", parts_str) < 0) write_err = 1;
    if (!write_err && fprintf(f, "---\n") < 0) write_err = 1;
    for (int i = 0; i < stored_line_count && !write_err; i++) {
        if (fprintf(f, "%s\n", encoded_lines[i]) < 0) write_err = 1;
    }
    if (write_err) {
        fclose(f);
        unlink(tmp_path);
        free(content_no_fl);
        for (int i = 0; i < stored_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(tmp_path);
        free(content_no_fl);
        for (int i = 0; i < stored_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    /* Postcondition: verify file-length matches actual .tmp size */
    struct stat st;
    int stat_rc = stat(tmp_path, &st);
    ASSERT_MSG(stat_rc == 0,
               "chat_truncate: stat failed on %s after write: %s",
               tmp_path, strerror(errno));
    ASSERT_MSG((int64_t)st.st_size == file_len,
               "chat_truncate postcondition: file-length header %" PRId64
               " != actual size %" PRId64 " — compute_file_length has a bug",
               file_len, (int64_t)st.st_size);

    /* Atomic rename */
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        free(content_no_fl);
        for (int i = 0; i < stored_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -1;
    }

    free(content_no_fl);
    for (int i = 0; i < stored_line_count; i++) free(encoded_lines[i]);
    free(encoded_lines);
    chat_state_free(&state);
    chat_lock_release(lock_fd);
    return 0;
}

int chat_poll(const char *path, const char *handle, int timeout_secs) {
    ASSERT_MSG(path != NULL, "chat_poll: path is NULL");
    ASSERT_MSG(handle != NULL, "chat_poll: handle is NULL");
    ASSERT_MSG(timeout_secs >= 0,
               "chat_poll: timeout_secs is negative: %d", timeout_secs);

    /* Get initial message count */
    chat_state_t state;
    memset(&state, 0, sizeof(state));
    if (chat_read(path, &state) < 0) return -1;
    int initial_count = state.message_count;
    chat_state_free(&state);

    for (int elapsed = 0; elapsed < timeout_secs; elapsed++) {
        sleep(1);

        if (chat_read(path, &state) < 0) {
            chat_state_free(&state); /* defensive: clean up partial allocation */
            return -1;
        }

        if (state.message_count > initial_count) {
            /* Check if any new message is from someone other than handle */
            for (int i = initial_count; i < state.message_count; i++) {
                if (strcmp(state.messages[i].handle, handle) != 0) {
                    chat_state_free(&state);
                    return 0; /* New message from other participant */
                }
            }
        }

        chat_state_free(&state);
    }

    return 3; /* Timeout */
}

void chat_state_free(chat_state_t *state) {
    if (!state) return;
    if (state->messages) {
        for (int i = 0; i < state->message_count; i++) {
            free(state->messages[i].content);
        }
        free(state->messages);
        state->messages = NULL;
    }
    state->message_count = 0;
    state->skipped_count = 0;
    state->participant_count = 0;
}

/* --- Read cursor tracking --- */

/* Build cursor file path from chat path: <chat_path>.cursors */
static void cursor_path(const char *chat_path, char *out, size_t out_sz) {
    int n = snprintf(out, out_sz, "%s.cursors", chat_path);
    ASSERT_MSG(n > 0 && n < (int)out_sz,
               "cursor_path: path overflow for %s", chat_path);
}

int chat_cursor_read(const char *chat_path, const char *handle) {
    ASSERT_MSG(chat_path != NULL, "chat_cursor_read: chat_path is NULL");
    ASSERT_MSG(handle != NULL, "chat_cursor_read: handle is NULL");

    ASSERT_MSG(handle[0] != '\0', "chat_cursor_read: handle is empty");
    char cpath[MAX_PATH_LEN];
    cursor_path(chat_path, cpath, sizeof(cpath));

    FILE *f = fopen(cpath, "r");
    if (!f) return -1;  /* No cursor file yet */

    char line[256];
    int result = -1;

    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        /* Extract key */
        size_t klen = (size_t)(eq - line);
        if (klen >= MAX_HANDLE_LEN) continue;

        char key[MAX_HANDLE_LEN];
        memcpy(key, line, klen);
        key[klen] = '\0';

        if (strcmp(key, handle) == 0) {
            if (safe_parse_int(eq + 1, &result) != 0) {
                fprintf(stderr, "warning: chat_cursor_read: invalid cursor value for handle '%s'\n", handle);
                result = -2;  /* Parse error: distinct from -1 (not found) */
            }
            break;
        }
    }

    fclose(f);
    return result;
}

int chat_cursor_write(const char *chat_path, const char *handle, int index) {
    ASSERT_MSG(chat_path != NULL, "chat_cursor_write: chat_path is NULL");
    ASSERT_MSG(handle != NULL, "chat_cursor_write: handle is NULL");
    ASSERT_MSG(handle[0] != '\0', "chat_cursor_write: handle is empty");
    ASSERT_MSG(index >= 0, "chat_cursor_write: index is negative: %d", index);

    char cpath[MAX_PATH_LEN];
    cursor_path(chat_path, cpath, sizeof(cpath));

    /* Lock the cursor file using the chat lock (same lock as chat_send) */
    int lock_fd = chat_lock_acquire(chat_path);
    if (lock_fd < 0) {
        fprintf(stderr, "warning: chat_cursor_write: lock acquisition failed for %s\n", chat_path);
        return -1;
    }

    /* Read existing cursors */
    char handles[MAX_PARTICIPANTS][MAX_HANDLE_LEN];
    int indices[MAX_PARTICIPANTS];
    int count = 0;
    int found = 0;

    FILE *f = fopen(cpath, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f) && count < MAX_PARTICIPANTS) {
            if (line[0] == '#' || line[0] == '\n') continue;

            char *eq = strchr(line, '=');
            if (!eq) continue;

            size_t klen = (size_t)(eq - line);
            if (klen >= MAX_HANDLE_LEN) continue;

            memcpy(handles[count], line, klen);
            handles[count][klen] = '\0';
            if (safe_parse_int(eq + 1, &indices[count]) != 0) {
                fprintf(stderr, "warning: chat_cursor_write: invalid cursor value, defaulting to 0\n");
                indices[count] = 0;
            }

            if (strcmp(handles[count], handle) == 0) {
                indices[count] = index;  /* Update existing */
                found = 1;
            }
            count++;
        }
        fclose(f);
    }

    /* Add new entry if not found */
    if (!found && count < MAX_PARTICIPANTS) {
        snprintf(handles[count], MAX_HANDLE_LEN, "%s", handle);
        indices[count] = index;
        count++;
    }

    /* Write back atomically */
    char tmp_path[MAX_PATH_LEN + 8];
    int n_tmp_path = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cpath);
    ASSERT_MSG(n_tmp_path >= 0 && (size_t)n_tmp_path < sizeof(tmp_path),
               "chat_cursor_write: tmp_path truncated for %s", cpath);

    int tmp_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (tmp_fd < 0) {
        chat_lock_release(lock_fd);
        return -1;
    }
    f = fdopen(tmp_fd, "w");
    if (!f) {
        close(tmp_fd);
        chat_lock_release(lock_fd);
        return -1;
    }

    int write_err = 0;
    if (fprintf(f, "# Read cursors — last-read message index per handle\n") < 0) write_err = 1;
    for (int i = 0; i < count && !write_err; i++) {
        if (fprintf(f, "%s=%d\n", handles[i], indices[i]) < 0) write_err = 1;
    }
    if (write_err || fclose(f) != 0) {
        fprintf(stderr, "warning: chat_cursor_write: write failed: %s\n", strerror(errno));
        unlink(tmp_path);
        chat_lock_release(lock_fd);
        return -1;
    }

    if (rename(tmp_path, cpath) != 0) {
        unlink(tmp_path);
        chat_lock_release(lock_fd);
        return -1;
    }

    chat_lock_release(lock_fd);
    return 0;
}

