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
    char *endptr;
    errno = 0;
    long long val = strtoll(str, &endptr, 10);
    if (errno != 0 || endptr == str || (*endptr != '\0' && *endptr != '\n' && *endptr != '\r')) return -1;
    *out = (int64_t)val;
    return 0;
}

static void get_timestamp(char *buf, size_t buf_size) {
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

    return candidate;
}

static int parse_participants(const char *line, participant_t *parts, int max_parts) {
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

    return count;
}

static void format_participants(const participant_t *parts, int count,
                                 char *buf, size_t buf_size) {
    size_t offset = 0;
    for (int i = 0; i < count; i++) {
        int written;
        if (i > 0) {
            written = snprintf(buf + offset, buf_size - offset, ", ");
            if (written > 0) offset += written;
        }
        written = snprintf(buf + offset, buf_size - offset,
                           "%s(%d)", parts[i].handle, parts[i].count);
        if (written > 0) offset += written;
    }
}

static int update_participants(participant_t *parts, int count,
                                const char *handle, int max_parts) {
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
    parts[count].count = 1;
    return count + 1;
}

/* --- Public API --- */

int chat_create(const char *path) {
    ASSERT_MSG(path != NULL, "chat_create: path is NULL");

    /* Check if file already exists */
    struct stat st;
    if (stat(path, &st) == 0) {
        return -1; /* Already exists */
    }

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

    /* Now write the actual file with file-length inserted */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -2;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return -2; }

    fprintf(f, "=== nbs-chat ===\n");
    fprintf(f, "last-writer: system\n");
    fprintf(f, "last-write: %s\n", timestamp);
    fprintf(f, "file-length: %" PRId64 "\n", file_len);
    fprintf(f, "participants: \n");
    fprintf(f, "---\n");
    if (fclose(f) != 0) {
        fprintf(stderr, "warning: chat_create: fclose failed: %s\n", strerror(errno));
        return -2;
    }

    /* Postcondition: verify file-length matches actual size */
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

    /* Temporary message storage */
    state->messages = malloc(sizeof(chat_message_t) * MAX_MESSAGES);
    if (!state->messages) {
        fclose(f);
        return -1;
    }
    state->message_count = 0;

    while (fgets(line, sizeof(line), f)) {
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
            /* Decode base64 message */
            size_t decoded_max = base64_decoded_size(len);
            unsigned char *decoded = malloc(decoded_max + 1);
            if (!decoded) {
                fprintf(stderr, "warning: chat_read: malloc failed for message %d, skipping\n", state->message_count);
                continue;
            }

            int decoded_len = base64_decode(line, len, decoded, decoded_max);
            if (decoded_len < 0) {
                free(decoded);
                continue;
            }
            decoded[decoded_len] = '\0';

            /* Parse "handle|EPOCH: content" or legacy "handle: content" */
            char *colon = strstr((char *)decoded, ": ");
            if (colon) {
                size_t prefix_len = colon - (char *)decoded;
                /* Check for pipe separator (timestamped format) */
                char *pipe = memchr((char *)decoded, '|', prefix_len);
                size_t handle_len;
                time_t msg_timestamp = 0;

                if (pipe) {
                    /* New format: handle|EPOCH: content */
                    handle_len = pipe - (char *)decoded;
                    /* Parse epoch between pipe and colon */
                    size_t epoch_len = colon - (pipe + 1);
                    if (epoch_len > 0 && epoch_len < 20) {
                        char epoch_buf[20];
                        memcpy(epoch_buf, pipe + 1, epoch_len);
                        epoch_buf[epoch_len] = '\0';
                        int64_t parsed_epoch;
                        if (safe_parse_int64(epoch_buf, &parsed_epoch) == 0 && parsed_epoch > 0) {
                            msg_timestamp = (time_t)parsed_epoch;
                        }
                    }
                } else {
                    /* Legacy format: handle: content */
                    handle_len = prefix_len;
                }

                if (handle_len < MAX_HANDLE_LEN && handle_len > 0) {
                    chat_message_t *msg = &state->messages[state->message_count];
                    strncpy(msg->handle, (char *)decoded, handle_len);
                    msg->handle[handle_len] = '\0';
                    msg->content = strdup(colon + 2);
                    if (!msg->content) {
                        fprintf(stderr, "warning: chat_read: strdup failed for message %d\n", state->message_count);
                        free(decoded);
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
    /* Format: handle|epoch: message */
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

    while (fgets(line_buf, sizeof(line_buf), f)) {
        size_t ll = strlen(line_buf);
        while (ll > 0 && (line_buf[ll-1] == '\n' || line_buf[ll-1] == '\r'))
            line_buf[--ll] = '\0';

        if (!past_delim) {
            if (strcmp(line_buf, "---") == 0 && encoded_line_count == 0) {
                /* This is the closing delimiter of the header */
                /* But we need to distinguish from "=== nbs-chat ===" */
                /* Check if we've seen the opening marker */
                past_delim = 1;
            }
            continue;
        }

        if (ll > 0) {
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
    fclose(f);

    /* Invariant: encoded_line_count must be non-negative */
    ASSERT_MSG(encoded_line_count >= 0,
               "chat_send: encoded_line_count went negative: %d", encoded_line_count);

    /* Calculate content without file-length for size computation */
    size_t content_size = header_len;
    for (int i = 0; i < encoded_line_count; i++) {
        content_size += strlen(encoded_lines[i]) + 1; /* +1 for \n */
    }
    content_size += strlen(encoded) + 1; /* new message + \n */

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

    /* Write the file with file-length inserted after last-write line */
    int wfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
    if (fprintf(f, "last-writer: %s\n", state.last_writer) < 0) write_err = 1;
    if (fprintf(f, "last-write: %s\n", state.last_write) < 0) write_err = 1;
    if (fprintf(f, "file-length: %" PRId64 "\n", file_len) < 0) write_err = 1;
    if (fprintf(f, "participants: %s\n", parts_str) < 0) write_err = 1;
    if (fprintf(f, "---\n") < 0) write_err = 1;
    for (int i = 0; i < encoded_line_count; i++) {
        if (fprintf(f, "%s\n", encoded_lines[i]) < 0) write_err = 1;
    }
    if (fprintf(f, "%s\n", encoded) < 0) write_err = 1;
    if (write_err) {
        fprintf(stderr, "error: chat_send: write failed for %s: %s\n",
                path, strerror(errno));
        fclose(f);
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
        free(content_no_fl);
        for (int i = 0; i < encoded_line_count; i++) free(encoded_lines[i]);
        free(encoded_lines);
        free(encoded);
        chat_state_free(&state);
        chat_lock_release(lock_fd);
        return -2;
    }

    /* Postcondition: verify file-length matches actual size */
    struct stat st;
    int stat_rc = stat(path, &st);
    ASSERT_MSG(stat_rc == 0,
               "chat_send: stat failed after write: %s", strerror(errno));
    ASSERT_MSG((int64_t)st.st_size == file_len,
               "chat_send postcondition: file-length header %" PRId64 " != actual size %" PRId64,
               file_len, (int64_t)st.st_size);

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
     * This is called AFTER lock release so chat_cursor_write can acquire
     * the lock independently. The race window (another message arriving
     * between send and cursor update) is benign: the cursor will be at
     * our message or later, which is correct either way. */
    int cw_rc = chat_cursor_write(path, handle, encoded_line_count);
    if (cw_rc < 0) {
        fprintf(stderr, "warning: chat_send: cursor-on-write failed for handle '%s'\n", handle);
        /* Non-fatal: the send succeeded, cursor update is best-effort */
    }

    return 0;
}

int chat_poll(const char *path, const char *handle, int timeout_secs) {
    ASSERT_MSG(path != NULL, "chat_poll: path is NULL");
    ASSERT_MSG(handle != NULL, "chat_poll: handle is NULL");
    ASSERT_MSG(timeout_secs >= 0,
               "chat_poll: timeout_secs is negative: %d", timeout_secs);

    /* Get initial message count */
    chat_state_t state;
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
                result = -1;
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
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cpath);

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

    fprintf(f, "# Read cursors — last-read message index per handle\n");
    for (int i = 0; i < count; i++) {
        fprintf(f, "%s=%d\n", handles[i], indices[i]);
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "warning: chat_cursor_write: fclose failed: %s\n", strerror(errno));
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

