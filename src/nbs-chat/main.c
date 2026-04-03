/*
 * main.c — nbs-chat CLI tool
 *
 * Usage: nbs-chat <command> [args...]
 *
 * Commands:
 *   create <file>                    Create new chat file
 *   send <file> <handle> <message>   Send a message
 *   warn <file> <message>            Post a [MEDIC-WARNING]
 *   error <file> <message>           Post a [SIDECAR-ERROR]
 *   read <file> [options]            Read messages
 *   poll <file> <handle> [options]   Wait for new message
 *   search <file> <pattern> [opts]   Search message history
 *   export <file> [options]          Export with ANSI colours
 *   delete <file> --after=<time>     Delete messages after time
 *   participants <file>              List participants
 *   help                             Show usage
 *
 * Exit codes:
 *   0 - Success
 *   1 - General error
 *   2 - File not found
 *   3 - Timeout (poll)
 *   4 - Invalid arguments
 */

#include "bus_bridge.h"
#include "chat_file.h"
#include "render.h"
#include "handle_styles.h"
#include "time_parse.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(void) {
    printf("nbs-chat: File-based AI-to-AI chat with atomic locking\n\n");
    printf("Usage: nbs-chat <command> [args...]\n\n");
    printf("Commands:\n");
    printf("  create <file>                    Create new chat file\n");
    printf("  send <file> <handle> <message>   Send a message\n");
    printf("  warn <file> <message>            Post a [MEDIC-WARNING]\n");
    printf("  error <file> <message>           Post a [SIDECAR-ERROR]\n");
    printf("  read <file> [options]            Read messages\n");
    printf("  poll <file> <handle> [options]   Wait for new message\n");
    printf("  search <file> <pattern> [opts]   Search message history\n");
    printf("  export <file> [options]          Export with ANSI colours\n");
    printf("  delete <file> --after=<time>     Delete messages after time\n");
    printf("  participants <file>              List participants and counts\n");
    printf("  count <file>                     Print authoritative message count\n");
    printf("  cursor-set <file> <handle> <N>   Atomically set cursor (lock-safe)\n");
    printf("  help                             Show this help\n\n");
    printf("Read options:\n");
    printf("  --last=N           Show only the last N messages\n");
    printf("  --since=<handle>   Show messages after last message from <handle>\n");
    printf("  --unread=<handle>  Show messages after read cursor for <handle>\n");
    printf("                     Auto-advances cursor after displaying\n");
    printf("  --after=<time>     Show messages after time (epoch, 2h, ISO 8601)\n");
    printf("  --before=<time>    Show messages before time\n\n");
    printf("Search options:\n");
    printf("  --handle=<name>  Only search messages from this handle\n");
    printf("  --after=<time>   Only search messages after time\n");
    printf("  --before=<time>  Only search messages before time\n\n");
    printf("Export options:\n");
    printf("  --last=N              Show only the last N messages\n");
    printf("  --from=N              Start from message N (0-based)\n");
    printf("  --to=N                End at message N (exclusive)\n");
    printf("  --handle=h1,h2,...    Only messages from these handles\n");
    printf("  --after=<time>        Messages after time\n");
    printf("  --before=<time>       Messages before time\n");
    printf("  --grep=<pattern>      Only messages matching pattern\n\n");
    printf("Delete options:\n");
    printf("  --after=<time>   Delete messages at or after time (required)\n");
    printf("  --dry-run        Show what would be deleted without modifying\n\n");
    printf("Time formats:\n");
    printf("  30s, 5m, 2h, 1d    Relative (ago from now)\n");
    printf("  1771834287          Epoch seconds (>=10 digits)\n");
    printf("  2026-02-23T00:11:27 ISO 8601 (local time)\n\n");
    printf("Poll options:\n");
    printf("  --timeout=N      Timeout in seconds (default: 10)\n\n");
    printf("Exit codes:\n");
    printf("  0 - Success\n");
    printf("  1 - General error\n");
    printf("  2 - File not found / already exists\n");
    printf("  3 - Timeout (poll only)\n");
    printf("  4 - Invalid arguments\n");
}

/*
 * resolve_path — Resolve a potentially relative path to absolute.
 *
 * Preconditions:
 *   - path != NULL
 *   - out_buf has at least MAX_PATH_LEN bytes
 *
 * Postconditions:
 *   - On success (returns 0): out_buf contains NUL-terminated absolute path
 *   - On failure (returns -1): error printed to stderr, out_buf undefined
 *   - If path is already absolute, it is copied verbatim (no resolution)
 */
static int resolve_path(const char *path, char *out_buf, const char *caller) {
    ASSERT_MSG(path != NULL, "%s: resolve_path called with NULL path", caller);
    ASSERT_MSG(out_buf != NULL, "%s: resolve_path called with NULL out_buf", caller);

    if (path[0] == '/') {
        /* Already absolute — copy directly */
        size_t len = strlen(path);
        if (len >= MAX_PATH_LEN) {
            fprintf(stderr, "Error: Path too long (%zu bytes, max %d): %.60s...\n",
                    len, MAX_PATH_LEN - 1, path);
            return -1;
        }
        memcpy(out_buf, path, len + 1);
        return 0;
    }

    /* Relative path — prepend cwd */
    char cwd[MAX_PATH_LEN];
    char *cwdp = getcwd(cwd, sizeof(cwd));
    if (cwdp == NULL) {
        fprintf(stderr, "Error: getcwd failed: %s — cannot resolve relative path '%s'\n",
                strerror(errno), path);
        return -1;
    }
    int snp_rc = snprintf(out_buf, MAX_PATH_LEN, "%s/%s", cwd, path);
    if (snp_rc < 0 || (size_t)snp_rc >= MAX_PATH_LEN) {
        fprintf(stderr, "Error: Resolved path too long (cwd='%s', file='%s', need %d, have %d)\n",
                cwd, path, snp_rc, MAX_PATH_LEN);
        return -1;
    }
    return 0;
}

static int cmd_create(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-chat create <file>\n");
        return 4;
    }
    ASSERT_MSG(argc >= 3, "cmd_create: argc %d after validation", argc);

    const char *path = argv[2];

    /* Precondition: path validated from argv */
    ASSERT_MSG(path != NULL, "cmd_create: path argument is NULL after argv extraction — this indicates an internal argument parsing error");

    /* Resolve to absolute path consistently (Violation 11 fix) */
    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_create") < 0) {
        return 4;
    }
    path = abs_path;

    int result = chat_create(path);
    if (result == -1) {
        fprintf(stderr, "Error: File already exists: %s\n", path);
        return 1;
    }
    if (result < 0) {
        fprintf(stderr, "Error: Could not create file: %s\n", path);
        return 1;
    }

    printf("Created: %s\n", path);
    return 0;
}

static int cmd_send(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: nbs-chat send <file> <handle> <message>\n");
        return 4;
    }
    ASSERT_MSG(argc >= 5, "cmd_send: argc %d after validation", argc);

    const char *path = argv[2];
    const char *handle = argv[3];
    const char *message = argv[4];

    /* Preconditions: args validated from argv */
    ASSERT_MSG(path != NULL, "cmd_send: path argument is NULL after argv extraction — this indicates an internal argument parsing error");
    ASSERT_MSG(handle != NULL, "cmd_send: handle argument is NULL after argv extraction — this indicates an internal argument parsing error");
    ASSERT_MSG(message != NULL, "cmd_send: message argument is NULL after argv extraction — this indicates an internal argument parsing error");

    /* Reject reserved handles. Handles containing '[' are reserved for
     * system messages (e.g. [MEDIC-WARNING]) and can only be produced
     * by dedicated subcommands, not by 'send'. */
    if (strchr(handle, '[') != NULL) {
        fprintf(stderr, "Error: handle '%s' contains reserved character '['\n",
                handle);
        return 4;
    }

    /* Resolve to absolute path consistently */
    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_send") < 0) {
        return 4;
    }
    path = abs_path;

    int result = chat_send(path, handle, message);
    if (result < 0) {
        fprintf(stderr, "Error: Failed to send message to '%s' (chat_send returned %d, errno=%d: %s)\n",
                path, result, errno, strerror(errno));
        return 1;
    }

    /* Publish bus events (if bus directory exists).
     * Bus failure is non-fatal — chat send already succeeded.
     * Return value intentionally discarded: bus_bridge_after_send
     * documents "returns 0 always" as a design invariant. */
    (void)bus_bridge_after_send(path, handle, message);

    return 0;
}

static int cmd_read(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-chat read <file> [--last=N] [--since=<handle>] [--unread=<handle>]\n");
        return 4;
    }
    ASSERT_MSG(argc >= 3, "cmd_read: argc %d after validation", argc);

    const char *path = argv[2];

    /* Precondition: path validated from argv */
    ASSERT_MSG(path != NULL, "cmd_read: path argument is NULL after argv extraction — this indicates an internal argument parsing error");

    /* Resolve to absolute path consistently */
    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_read") < 0) {
        return 4;
    }
    path = abs_path;

    int last_n = -1;
    int offset_n = 0;
    const char *since_handle = NULL;
    const char *unread_handle = NULL;
    time_t after_time = 0;
    time_t before_time = 0;

    /* Parse options */
    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "--last=", 7) == 0) {
            char *endptr;
            errno = 0;
            long val = strtol(argv[i] + 7, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || val < 0 || val > INT_MAX) {
                fprintf(stderr, "Error: Invalid --last value: %s\n", argv[i] + 7);
                return 4;
            }
            last_n = (int)val;
            /* Note: --last=0 is valid and means "show zero messages" */
        } else if (strncmp(argv[i], "--offset=", 9) == 0) {
            char *endptr;
            errno = 0;
            long val = strtol(argv[i] + 9, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || val < 0 || val > INT_MAX) {
                fprintf(stderr, "Error: Invalid --offset value: %s\n", argv[i] + 9);
                return 4;
            }
            offset_n = (int)val;
        } else if (strncmp(argv[i], "--since=", 8) == 0) {
            since_handle = argv[i] + 8;
            if (since_handle[0] == '\0') {
                fprintf(stderr, "Error: --since= requires a non-empty handle value\n");
                return 4;
            }
        } else if (strncmp(argv[i], "--unread=", 9) == 0) {
            unread_handle = argv[i] + 9;
            if (unread_handle[0] == '\0') {
                fprintf(stderr, "Error: --unread= requires a non-empty handle value\n");
                return 4;
            }
        } else if (strncmp(argv[i], "--after=", 8) == 0) {
            if (parse_timespec(argv[i] + 8, &after_time) < 0) {
                fprintf(stderr, "Error: Invalid --after value: %s\n", argv[i] + 8);
                return 4;
            }
        } else if (strncmp(argv[i], "--before=", 9) == 0) {
            if (parse_timespec(argv[i] + 9, &before_time) < 0) {
                fprintf(stderr, "Error: Invalid --before value: %s\n", argv[i] + 9);
                return 4;
            }
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 4;
        }
    }

    /* B10 fix: removed access() TOCTOU pre-check. Let chat_read fail and
     * inspect errno to distinguish file-not-found from other errors. */
    chat_state_t state;
    int read_rc = chat_read(path, &state);
    if (read_rc < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: Chat file not found: %s\n", path);
            return 2;
        }
        fprintf(stderr, "Error: Failed to read chat file '%s' (chat_read returned %d, errno=%d: %s)\n",
                path, read_rc, errno, strerror(errno));
        return 1;
    }
    ASSERT_MSG(state.message_count == 0 || state.messages != NULL,
               "cmd_read: chat_read returned 0 but messages is NULL with message_count=%d",
               state.message_count);

    if (state.skipped_count > 0) {
        fprintf(stderr, "warning: %d message(s) skipped (decode failure)\n",
                state.skipped_count);
    }

    int start = 0;
    int end = state.message_count;

    /* Violation 4 fix: assert bounds before array indexing */
    ASSERT_MSG(start >= 0 && start <= end,
               "cmd_read: start=%d end=%d out of bounds after filter computation", start, end);

    /* Apply --unread filter (takes precedence over --since) */
    if (unread_handle) {
        int cursor = chat_cursor_read(path, unread_handle);
        /* Treat any negative value (no cursor or parse error) as "show all" */
        if (cursor < -1) cursor = -1;
        /* HARDENING: guard against integer overflow on cursor + 1 */
        ASSERT_MSG(cursor < INT_MAX,
                   "cmd_read: cursor value %d would overflow on increment", cursor);
        /* cursor is last-read index; show messages after it */
        start = cursor + 1;  /* -1 + 1 = 0 if no cursor exists (show all) */
        /* Clamp: if file shrunk since cursor was written (e.g. after archive),
         * start may exceed message_count. Clamp cursor in the file so it
         * doesn't stay impossibly high (Scenario #7 mitigation). */
        if (start > end) {
            int clamped = (state.message_count > 0) ? state.message_count - 1 : 0;
            fprintf(stderr, "warning: read cursor for '%s' (%d) exceeds message count (%d), clamping to %d\n",
                    unread_handle, cursor, state.message_count, clamped);
            /* Write clamped cursor back to file */
            int clamp_rc = chat_cursor_write(path, unread_handle, clamped);
            if (clamp_rc < 0) {
                fprintf(stderr, "warning: failed to write clamped cursor for '%s'\n", unread_handle);
            }
            start = end;
        }
    } else if (since_handle) {
        /* Apply --since filter */
        /* Find last message from since_handle, show messages after it */
        int last_from = -1;
        for (int i = 0; i < state.message_count; i++) {
            if (strcmp(state.messages[i].handle, since_handle) == 0) {
                last_from = i;
            }
        }
        if (last_from >= 0) {
            start = last_from + 1;
        }
    }

    /* Apply --after filter: advance start to first message at or after time */
    if (after_time > 0) {
        while (start < end && state.messages[start].timestamp < after_time) {
            start++;
        }
    }

    /* Apply --before filter: retreat end to exclude messages after time */
    if (before_time > 0) {
        while (end > start && state.messages[end - 1].timestamp > before_time) {
            end--;
        }
    }

    /* V7 fix: Apply --offset filter with range warning */
    if (offset_n > 0) {
        if (end - start <= offset_n) {
            fprintf(stderr, "warning: --offset=%d exceeds available message count (%d), showing nothing\n",
                    offset_n, end - start);
            start = end;
        } else {
            end -= offset_n;
        }
    }

    /* Apply --last filter */
    if (last_n >= 0 && end - start > last_n) {
        start = end - last_n;
    }

    /* Postcondition: bounds validated before array access */
    ASSERT_MSG(start >= 0 && start <= end && end <= state.message_count,
               "cmd_read: array bounds violated: start=%d end=%d message_count=%d",
               start, end, state.message_count);

    /* Print messages */
    for (int i = start; i < end; i++) {
        if (state.messages[i].timestamp > 0) {
            struct tm tm_buf;
            struct tm *tm = gmtime_r(&state.messages[i].timestamp, &tm_buf);
            if (tm) {
                char ts[32];
                strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
                printf("[%s] %s: %s\n", ts,
                       state.messages[i].handle, state.messages[i].content);
            } else {
                printf("%s: %s\n", state.messages[i].handle, state.messages[i].content);
            }
        } else {
            printf("%s: %s\n", state.messages[i].handle, state.messages[i].content);
        }
    }

    /* V9 fix: Advance read cursor only when messages were actually displayed.
     * Previously used (end > 0) which advances cursor even when start == end
     * (i.e., all messages filtered out by --last=0, --before, etc.). */
    if (unread_handle && end > start) {
        int cw_rc = chat_cursor_write(path, unread_handle, end - 1);
        if (cw_rc < 0) {
            fprintf(stderr, "warning: failed to update read cursor for '%s'\n", unread_handle);
        }
    }

    chat_state_free(&state);
    return 0;
}

static int cmd_poll(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: nbs-chat poll <file> <handle> [--timeout=N]\n");
        return 4;
    }
    ASSERT_MSG(argc >= 4, "cmd_poll: argc %d after validation", argc);

    const char *path = argv[2];
    const char *handle = argv[3];

    /* Preconditions: args validated from argv */
    ASSERT_MSG(path != NULL, "cmd_poll: path argument is NULL after argv extraction — this indicates an internal argument parsing error");
    ASSERT_MSG(handle != NULL, "cmd_poll: handle argument is NULL after argv extraction — this indicates an internal argument parsing error");

    /* Resolve to absolute path consistently */
    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_poll") < 0) {
        return 4;
    }
    path = abs_path;

    int timeout = 10;

    for (int i = 4; i < argc; i++) {
        if (strncmp(argv[i], "--timeout=", 10) == 0) {
            char *endptr;
            errno = 0;
            long val = strtol(argv[i] + 10, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || val < 0 || val > INT_MAX) {
                fprintf(stderr, "Error: Invalid --timeout value: %s\n", argv[i] + 10);
                return 4;
            }
            timeout = (int)val;
            /* Note: --timeout=0 is valid and means "check once, return immediately" */
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 4;
        }
    }

    /* B10 fix: removed access() TOCTOU pre-check. Let chat_poll fail and
     * inspect errno to distinguish file-not-found from other errors. */
    int result = chat_poll(path, handle, timeout);
    if (result == 3) return 3; /* Timeout */
    if (result < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: Chat file not found: %s\n", path);
            return 2;
        }
        fprintf(stderr, "Error: Poll failed on '%s' (chat_poll returned %d, errno=%d: %s)\n",
                path, result, errno, strerror(errno));
        return 1;
    }
    /* chat_poll documents return values: 0 (success), 3 (timeout), -1 (error).
     * Any other value is a contract violation. */
    ASSERT_MSG(result == 0, "cmd_poll: unexpected chat_poll return value %d (expected 0, 3, or <0)", result);

    /* Print new messages */
    chat_state_t state;
    int read_rc = chat_read(path, &state);
    if (read_rc < 0) {
        fprintf(stderr, "Error: Poll succeeded but failed to read chat file '%s' "
                "(chat_read returned %d, errno=%d: %s)\n",
                path, read_rc, errno, strerror(errno));
        return 1;
    }
    /* V2 fix: assert messages pointer is valid when count > 0 */
    ASSERT_MSG(state.message_count == 0 || state.messages != NULL,
               "cmd_poll: chat_read succeeded but messages is NULL with count=%d",
               state.message_count);

    /* V6 fix: Print the last message from someone other than the polling handle.
     * Track whether a message was found to verify chat_poll's postcondition.
     * Known TOCTOU window (V11): between chat_poll and chat_read, another
     * process could modify the file. The assertion below catches this. */
    int found = 0;
    for (int i = state.message_count - 1; i >= 0; i--) {
        if (strcmp(state.messages[i].handle, handle) != 0) {
            if (state.messages[i].timestamp > 0) {
                struct tm tm_buf;
                struct tm *tm = gmtime_r(&state.messages[i].timestamp, &tm_buf);
                if (tm) {
                    char ts[32];
                    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
                    printf("[%s] %s: %s\n", ts,
                           state.messages[i].handle, state.messages[i].content);
                } else {
                    printf("%s: %s\n", state.messages[i].handle, state.messages[i].content);
                }
            } else {
                printf("%s: %s\n", state.messages[i].handle,
                       state.messages[i].content);
            }
            found = 1;
            break;
        }
    }
    /* B11 fix: replaced ASSERT_MSG with conditional warning + return.
     * Between chat_poll returning 0 and the subsequent chat_read, another
     * process could delete the message (legitimate race). Aborting via
     * assert on a race condition is a bug, not a safety net. */
    if (!found) {
        fprintf(stderr, "warning: cmd_poll: chat_poll returned 0 but no message from another "
                "handle found in '%s' (possible race: message deleted between poll and read)\n",
                path);
        chat_state_free(&state);
        return 1;
    }
    chat_state_free(&state);

    return 0;
}

static int cmd_participants(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-chat participants <file>\n");
        return 4;
    }
    ASSERT_MSG(argc >= 3, "cmd_participants: argc %d after validation", argc);

    const char *path = argv[2];

    /* Precondition: path validated from argv */
    ASSERT_MSG(path != NULL, "cmd_participants: path argument is NULL after argv extraction — this indicates an internal argument parsing error");

    /* Resolve to absolute path consistently */
    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_participants") < 0) {
        return 4;
    }
    path = abs_path;

    /* B10 fix: removed access() TOCTOU pre-check. Let chat_read fail and
     * inspect errno to distinguish file-not-found from other errors. */
    chat_state_t state;
    int read_rc = chat_read(path, &state);
    if (read_rc < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: Chat file not found: %s\n", path);
            return 2;
        }
        fprintf(stderr, "Error: Failed to read chat file '%s' (chat_read returned %d, errno=%d: %s)\n",
                path, read_rc, errno, strerror(errno));
        return 1;
    }

    /* V10 fix: bounds assertion on participant_count */
    ASSERT_MSG(state.participant_count >= 0 && state.participant_count <= MAX_PARTICIPANTS,
               "cmd_participants: participant_count=%d out of bounds [0, %d]",
               state.participant_count, MAX_PARTICIPANTS);

    for (int i = 0; i < state.participant_count; i++) {
        printf("%-24s %d messages\n", state.participants[i].handle,
               state.participants[i].count);
    }

    chat_state_free(&state);
    return 0;
}

/*
 * strcasestr_portable — Case-insensitive substring search.
 *
 * Preconditions:
 *   - haystack != NULL
 *   - needle != NULL
 *
 * Returns pointer to first occurrence, or NULL if not found.
 */
static const char *strcasestr_portable(const char *haystack, const char *needle) {
    ASSERT_MSG(haystack != NULL, "strcasestr_portable: haystack is NULL");
    ASSERT_MSG(needle != NULL, "strcasestr_portable: needle is NULL");

    if (needle[0] == '\0') return haystack;

    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) {
            return p;
        }
    }
    return NULL;
}

static int cmd_search(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: nbs-chat search <file> <pattern> [--handle=<name>] [--after=<time>] [--before=<time>] [--include-archives]\n");
        return 4;
    }
    ASSERT_MSG(argc >= 4, "cmd_search: argc %d after validation", argc);

    const char *path = argv[2];
    const char *pattern = argv[3];
    const char *filter_handle = NULL;
    time_t after_time = 0;
    time_t before_time = 0;
    int include_archives = 0;

    /* Preconditions: args validated from argv */
    ASSERT_MSG(path != NULL, "cmd_search: path argument is NULL");
    ASSERT_MSG(pattern != NULL, "cmd_search: pattern argument is NULL");

    /* Resolve to absolute path consistently */
    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_search") < 0) {
        return 4;
    }
    path = abs_path;

    /* Parse options */
    for (int i = 4; i < argc; i++) {
        if (strncmp(argv[i], "--handle=", 9) == 0) {
            filter_handle = argv[i] + 9;
            if (filter_handle[0] == '\0') {
                fprintf(stderr, "Warning: --handle= value is empty, ignoring\n");
                filter_handle = NULL;
            }
        } else if (strncmp(argv[i], "--after=", 8) == 0) {
            if (parse_timespec(argv[i] + 8, &after_time) < 0) {
                fprintf(stderr, "Error: Invalid --after value: %s\n", argv[i] + 8);
                return 4;
            }
        } else if (strncmp(argv[i], "--before=", 9) == 0) {
            if (parse_timespec(argv[i] + 9, &before_time) < 0) {
                fprintf(stderr, "Error: Invalid --before value: %s\n", argv[i] + 9);
                return 4;
            }
        } else if (strcmp(argv[i], "--include-archives") == 0) {
            include_archives = 1;
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 4;
        }
    }

    /* B10 fix: removed access() TOCTOU pre-check. Let chat_read fail and
     * inspect errno to distinguish file-not-found from other errors. */
    chat_state_t state;
    int read_rc = chat_read(path, &state);
    if (read_rc < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: Chat file not found: %s\n", path);
            return 2;
        }
        fprintf(stderr, "Error: Failed to read chat file '%s' (chat_read returned %d, errno=%d: %s)\n",
                path, read_rc, errno, strerror(errno));
        return 1;
    }

    /* V3 fix: assert messages pointer is valid when count > 0 */
    ASSERT_MSG(state.message_count == 0 || state.messages != NULL,
               "cmd_search: chat_read succeeded but messages is NULL with count=%d",
               state.message_count);

    int match_count = 0;
    for (int i = 0; i < state.message_count; i++) {
        /* Apply handle filter if specified */
        if (filter_handle && strcmp(state.messages[i].handle, filter_handle) != 0) {
            continue;
        }

        /* Apply time filters */
        if (after_time > 0 && state.messages[i].timestamp < after_time) {
            continue;
        }
        if (before_time > 0 && state.messages[i].timestamp > before_time) {
            continue;
        }

        /* Case-insensitive search in message content */
        if (strcasestr_portable(state.messages[i].content, pattern) != NULL) {
            if (state.messages[i].timestamp > 0) {
                struct tm tm_buf;
                struct tm *tm = gmtime_r(&state.messages[i].timestamp, &tm_buf);
                if (tm) {
                    char ts[32];
                    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
                    printf("[%d] [%s] %s: %s\n", i, ts,
                           state.messages[i].handle, state.messages[i].content);
                } else {
                    printf("[%d] %s: %s\n", i, state.messages[i].handle,
                           state.messages[i].content);
                }
            } else {
                printf("[%d] %s: %s\n", i, state.messages[i].handle,
                       state.messages[i].content);
            }
            match_count++;
        }
    }

    chat_state_free(&state);

    /* Search archive files if requested */
    if (include_archives) {
        /* Derive archive glob pattern from the live file path.
         * For path "/dir/foo.chat", glob for "/dir/foo-*-archive.chat". */
        char dir[MAX_PATH_LEN];
        char base[MAX_PATH_LEN];
        snprintf(dir, sizeof(dir), "%s", path);
        snprintf(base, sizeof(base), "%s", path);

        /* Find last '/' to split dir and basename */
        char *last_slash = strrchr(dir, '/');
        const char *dirname_part;
        const char *basename_part;
        if (last_slash) {
            *last_slash = '\0';
            dirname_part = dir;
            basename_part = last_slash + 1;
            /* Also update base to just the basename */
            memmove(base, basename_part, strlen(basename_part) + 1);
        } else {
            dirname_part = ".";
            /* base already holds the full filename */
        }

        /* Strip .chat extension from basename to form glob stem */
        char stem[MAX_PATH_LEN];
        snprintf(stem, sizeof(stem), "%s", base);
        char *ext = strstr(stem, ".chat");
        if (ext && ext[5] == '\0') {
            *ext = '\0';
        }

        char glob_pattern[MAX_PATH_LEN * 2];
        int gp_len = snprintf(glob_pattern, sizeof(glob_pattern), "%s/%s-*-archive.chat",
                              dirname_part, stem);
        if (gp_len < 0 || (size_t)gp_len >= sizeof(glob_pattern)) {
            fprintf(stderr, "Error: Archive glob pattern too long\n");
            return 1;
        }

        glob_t globbuf;
        int grc = glob(glob_pattern, 0, NULL, &globbuf);
        if (grc == 0) {
            /* Sort reverse-alphabetically (newest timestamps first) */
            for (size_t a = 0; a < globbuf.gl_pathc; a++) {
                for (size_t b = a + 1; b < globbuf.gl_pathc; b++) {
                    if (strcmp(globbuf.gl_pathv[a], globbuf.gl_pathv[b]) < 0) {
                        char *tmp = globbuf.gl_pathv[a];
                        globbuf.gl_pathv[a] = globbuf.gl_pathv[b];
                        globbuf.gl_pathv[b] = tmp;
                    }
                }
            }

            for (size_t gi = 0; gi < globbuf.gl_pathc; gi++) {
                const char *archive_path = globbuf.gl_pathv[gi];

                /* Extract just the filename for the prefix label */
                const char *archive_name = strrchr(archive_path, '/');
                archive_name = archive_name ? archive_name + 1 : archive_path;

                chat_state_t astate;
                int arc = chat_read(archive_path, &astate);
                if (arc < 0) {
                    fprintf(stderr, "Warning: Could not read archive %s, skipping\n",
                            archive_path);
                    continue;
                }

                ASSERT_MSG(astate.message_count == 0 || astate.messages != NULL,
                           "cmd_search: chat_read succeeded but messages is NULL with count=%d (archive %s)",
                           astate.message_count, archive_path);

                for (int ai = 0; ai < astate.message_count; ai++) {
                    if (filter_handle && strcmp(astate.messages[ai].handle, filter_handle) != 0) {
                        continue;
                    }
                    if (after_time > 0 && astate.messages[ai].timestamp < after_time) {
                        continue;
                    }
                    if (before_time > 0 && astate.messages[ai].timestamp > before_time) {
                        continue;
                    }

                    if (strcasestr_portable(astate.messages[ai].content, pattern) != NULL) {
                        if (astate.messages[ai].timestamp > 0) {
                            struct tm tm_buf;
                            struct tm *tm = gmtime_r(&astate.messages[ai].timestamp, &tm_buf);
                            if (tm) {
                                char ts[32];
                                strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
                                printf("[%s] [%d] [%s] %s: %s\n", archive_name, ai, ts,
                                       astate.messages[ai].handle, astate.messages[ai].content);
                            } else {
                                printf("[%s] [%d] %s: %s\n", archive_name, ai,
                                       astate.messages[ai].handle, astate.messages[ai].content);
                            }
                        } else {
                            printf("[%s] [%d] %s: %s\n", archive_name, ai,
                                   astate.messages[ai].handle, astate.messages[ai].content);
                        }
                        match_count++;
                    }
                }

                chat_state_free(&astate);
            }

            globfree(&globbuf);
        } else if (grc != GLOB_NOMATCH) {
            fprintf(stderr, "Warning: glob() failed for pattern %s\n", glob_pattern);
        }
    }

    if (match_count == 0) {
        /* No matches — exit code 0 (not an error, just no results) */
        printf("No matches found.\n");
    }

    return 0;
}

/*
 * handle_match — Check if handle is in a comma-separated list.
 *
 * Preconditions:
 *   - handle != NULL
 *   - list != NULL (comma-separated handle names, no spaces)
 *
 * Returns 1 if handle matches any entry in the list, 0 otherwise.
 */
static int handle_match(const char *handle, const char *list) {
    ASSERT_MSG(handle != NULL, "handle_match: handle is NULL");
    ASSERT_MSG(list != NULL, "handle_match: list is NULL");

    size_t hlen = strlen(handle);
    const char *p = list;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t entry_len = comma ? (size_t)(comma - p) : strlen(p);
        if (entry_len == hlen && strncmp(p, handle, hlen) == 0) {
            return 1;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

static int cmd_export(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-chat export <file> [--last=N] [--from=N] [--to=N] "
                "[--handle=h1,h2] [--after=<time>] [--before=<time>] [--grep=<pattern>]\n");
        return 4;
    }

    const char *path = argv[2];
    ASSERT_MSG(path != NULL, "cmd_export: path is NULL");

    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_export") < 0) {
        return 4;
    }
    path = abs_path;

    int last_n = -1;
    int from_n = -1;
    int to_n = -1;
    const char *handle_filter = NULL;
    const char *grep_pattern = NULL;
    time_t after_time = 0;
    time_t before_time = 0;

    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "--last=", 7) == 0) {
            char *endptr;
            errno = 0;
            long val = strtol(argv[i] + 7, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || val < 0 || val > INT_MAX) {
                fprintf(stderr, "Error: Invalid --last value: %s\n", argv[i] + 7);
                return 4;
            }
            last_n = (int)val;
        } else if (strncmp(argv[i], "--from=", 7) == 0) {
            char *endptr;
            errno = 0;
            long val = strtol(argv[i] + 7, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || val < 0 || val > INT_MAX) {
                fprintf(stderr, "Error: Invalid --from value: %s\n", argv[i] + 7);
                return 4;
            }
            from_n = (int)val;
        } else if (strncmp(argv[i], "--to=", 5) == 0) {
            char *endptr;
            errno = 0;
            long val = strtol(argv[i] + 5, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || val < 0 || val > INT_MAX) {
                fprintf(stderr, "Error: Invalid --to value: %s\n", argv[i] + 5);
                return 4;
            }
            to_n = (int)val;
        } else if (strncmp(argv[i], "--handle=", 9) == 0) {
            handle_filter = argv[i] + 9;
            if (handle_filter[0] == '\0') {
                fprintf(stderr, "Error: --handle= requires a value\n");
                return 4;
            }
        } else if (strncmp(argv[i], "--after=", 8) == 0) {
            if (parse_timespec(argv[i] + 8, &after_time) < 0) {
                fprintf(stderr, "Error: Invalid --after value: %s\n", argv[i] + 8);
                return 4;
            }
        } else if (strncmp(argv[i], "--before=", 9) == 0) {
            if (parse_timespec(argv[i] + 9, &before_time) < 0) {
                fprintf(stderr, "Error: Invalid --before value: %s\n", argv[i] + 9);
                return 4;
            }
        } else if (strncmp(argv[i], "--grep=", 7) == 0) {
            grep_pattern = argv[i] + 7;
            if (grep_pattern[0] == '\0') {
                fprintf(stderr, "Error: --grep= requires a pattern\n");
                return 4;
            }
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 4;
        }
    }

    chat_state_t state;
    int read_rc = chat_read(path, &state);
    if (read_rc < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: Chat file not found: %s\n", path);
            return 2;
        }
        fprintf(stderr, "Error: Failed to read chat file '%s' (chat_read returned %d, errno=%d: %s)\n",
                path, read_rc, errno, strerror(errno));
        return 1;
    }
    ASSERT_MSG(state.message_count == 0 || state.messages != NULL,
               "cmd_export: chat_read returned 0 but messages is NULL with message_count=%d",
               state.message_count);

    if (state.skipped_count > 0) {
        fprintf(stderr, "warning: %d message(s) skipped (decode failure)\n",
                state.skipped_count);
    }

    /* Compute index range */
    int start = 0;
    int end = state.message_count;

    if (from_n >= 0) {
        start = from_n;
        if (start > end) start = end;
    }
    if (to_n >= 0) {
        if (to_n < end) end = to_n;
    }

    /* Apply time filters */
    if (after_time > 0) {
        while (start < end && state.messages[start].timestamp < after_time) {
            start++;
        }
    }
    if (before_time > 0) {
        while (end > start && state.messages[end - 1].timestamp > before_time) {
            end--;
        }
    }

    /* Apply --last (after other range filters) */
    if (last_n >= 0 && end - start > last_n) {
        start = end - last_n;
    }

    ASSERT_MSG(start >= 0 && start <= end && end <= state.message_count,
               "cmd_export: bounds violated: start=%d end=%d message_count=%d",
               start, end, state.message_count);

    /* Render with per-message filters (handle, grep) */
    render_init();

    for (int i = start; i < end; i++) {
        if (handle_filter && !handle_match(state.messages[i].handle, handle_filter)) {
            continue;
        }
        if (grep_pattern &&
            !strcasestr_portable(state.messages[i].content, grep_pattern)) {
            continue;
        }
        const nbs_style_t *bracket_style = handle_style_lookup(state.messages[i].handle);
        if (bracket_style) {
            render_message_bracket(state.messages[i].handle,
                                   state.messages[i].content,
                                   state.messages[i].timestamp,
                                   bracket_style, stdout);
        } else {
            render_message(state.messages[i].handle,
                           state.messages[i].content,
                           state.messages[i].timestamp, stdout);
        }
    }

    chat_state_free(&state);
    return 0;
}

static int cmd_delete(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-chat delete <file> --after=<time> [--dry-run]\n");
        return 4;
    }
    ASSERT_MSG(argc >= 3, "cmd_delete: argc %d after validation", argc);

    const char *path = argv[2];

    ASSERT_MSG(path != NULL, "cmd_delete: path argument is NULL");

    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_delete") < 0) {
        return 4;
    }
    path = abs_path;

    time_t after_time = 0;
    int dry_run = 0;

    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "--after=", 8) == 0) {
            if (parse_timespec(argv[i] + 8, &after_time) < 0) {
                fprintf(stderr, "Error: Invalid --after value: %s\n", argv[i] + 8);
                return 4;
            }
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 4;
        }
    }

    if (after_time == 0) {
        fprintf(stderr, "Error: --after=<time> is required for delete\n");
        return 4;
    }

    /* B10 fix: removed access() TOCTOU pre-check. Let chat_read fail and
     * inspect errno to distinguish file-not-found from other errors. */

    /* Read file to find truncation point */
    chat_state_t state;
    int read_rc = chat_read(path, &state);
    if (read_rc < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: Chat file not found: %s\n", path);
            return 2;
        }
        fprintf(stderr, "Error: Failed to read chat file: %s\n", path);
        return 1;
    }

    /* V4 fix: assert messages pointer is valid when count > 0 */
    ASSERT_MSG(state.message_count == 0 || state.messages != NULL,
               "cmd_delete: chat_read succeeded but messages is NULL with count=%d",
               state.message_count);

    /* Find first message at or after the cutoff time */
    int truncate_at = state.message_count;  /* default: nothing to delete */
    for (int i = 0; i < state.message_count; i++) {
        if (state.messages[i].timestamp >= after_time) {
            truncate_at = i;
            break;
        }
    }

    int to_delete = state.message_count - truncate_at;

    if (to_delete == 0) {
        printf("No messages to delete (0 messages at or after the specified time)\n");
        chat_state_free(&state);
        return 0;
    }

    if (dry_run) {
        printf("Would delete %d message(s) (keeping %d)\n",
               to_delete, truncate_at);
        chat_state_free(&state);
        return 0;
    }

    chat_state_free(&state);

    int rc = chat_truncate(path, truncate_at);
    if (rc < 0) {
        fprintf(stderr, "Error: Failed to truncate chat file\n");
        return 1;
    }

    /* V8 fix: postcondition verification -- re-read to confirm truncation */
    chat_state_t verify_state;
    if (chat_read(path, &verify_state) == 0) {
        ASSERT_MSG(verify_state.message_count <= truncate_at,
                   "cmd_delete: postcondition failed: expected <= %d messages after truncate, got %d",
                   truncate_at, verify_state.message_count);
        chat_state_free(&verify_state);
    } else {
        /* HARDENING: postcondition verification failed — do not silently skip.
         * The truncation itself succeeded, so we report success but warn that
         * we could not verify the result. */
        fprintf(stderr, "warning: cmd_delete: postcondition verification failed "
                "(chat_read returned error after successful truncate, errno=%d: %s)\n",
                errno, strerror(errno));
    }

    printf("Deleted %d message(s) (kept %d)\n", to_delete, truncate_at);
    return 0;
}

/*
 * cmd_warn — Post a medic warning to a chat file.
 *
 * Uses the reserved handle [MEDIC-WARNING] which cannot be produced by
 * cmd_send (brackets are rejected). This enforces at the binary level
 * that only the warn subcommand can create these messages.
 */
static int cmd_warn(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: nbs-chat warn <file> <message>\n");
        return 4;
    }

    const char *path = argv[2];
    const char *message = argv[3];
    const char *handle = "[MEDIC-WARNING]";

    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_warn") < 0) {
        return 4;
    }
    path = abs_path;

    int result = chat_send(path, handle, message);
    if (result < 0) {
        fprintf(stderr, "Error: Failed to send warning to '%s'\n", path);
        return 1;
    }

    return 0;
}

/*
 * cmd_error — Post a sidecar error to a chat file.
 *
 * Uses the reserved handle [SIDECAR-ERROR] which cannot be produced by
 * cmd_send (brackets are rejected). This enforces at the binary level
 * that only the error subcommand can create these messages.
 */
static int cmd_error(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: nbs-chat error <file> <message>\n");
        return 4;
    }

    const char *path = argv[2];
    const char *message = argv[3];
    const char *handle = "[SIDECAR-ERROR]";

    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_error") < 0) {
        return 4;
    }
    path = abs_path;

    int result = chat_send(path, handle, message);
    if (result < 0) {
        fprintf(stderr, "Error: Failed to send error to '%s'\n", path);
        return 1;
    }

    return 0;
}

/*
 * cmd_count — Print the authoritative message count for a chat file.
 *
 * Uses separator-based counting (lines after "---"), not line count
 * minus a hardcoded header offset. This avoids the wrong-formula bug
 * (Scenario #8) where header format changes break all cursor arithmetic.
 *
 * Exit: 0 on success (prints count to stdout), 1 on error, 2 not found.
 */
static int cmd_count(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-chat count <file>\n");
        return 4;
    }

    const char *path = argv[2];
    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_count") < 0) {
        return 4;
    }
    path = abs_path;

    /* Open file and count lines after the --- separator */
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: Chat file not found: %s\n", path);
            return 2;
        }
        fprintf(stderr, "Error: Cannot open chat file: %s\n", path);
        return 1;
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    int found_separator = 0;
    int count = 0;

    while ((line_len = getline(&line, &line_cap, f)) != -1) {
        /* Strip trailing newline */
        if (line_len > 0 && line[line_len - 1] == '\n')
            line[--line_len] = '\0';

        if (!found_separator) {
            if (strcmp(line, "---") == 0)
                found_separator = 1;
            continue;
        }

        /* After separator: count non-empty lines (one per message) */
        if (line_len > 0)
            count++;
    }

    free(line);
    fclose(f);

    printf("%d\n", count);
    return 0;
}

/*
 * cmd_cursor_set — Atomically set a handle's cursor value.
 *
 * Uses chat_cursor_write() which acquires the chat lock, ensuring
 * safe concurrent access. This replaces sed -i in shell scripts
 * (Scenario #2: concurrent cursor write race).
 *
 * Exit: 0 on success, 1 on error, 2 not found, 4 invalid args.
 */
static int cmd_cursor_set(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: nbs-chat cursor-set <file> <handle> <value>\n");
        return 4;
    }

    const char *path = argv[2];
    const char *handle = argv[3];
    const char *value_str = argv[4];

    char abs_path[MAX_PATH_LEN];
    if (resolve_path(path, abs_path, "cmd_cursor_set") < 0) {
        return 4;
    }
    path = abs_path;

    /* Parse value */
    char *endptr;
    errno = 0;
    long value = strtol(value_str, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || value < 0 || value > INT_MAX - 1) {
        fprintf(stderr, "Error: Invalid cursor value: %s (must be 0..%d)\n",
                value_str, INT_MAX - 1);
        return 4;
    }

    /* Validate handle */
    if (handle[0] == '\0') {
        fprintf(stderr, "Error: Handle must not be empty\n");
        return 4;
    }

    /* Check chat file exists */
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            fprintf(stderr, "Error: Chat file not found: %s\n", path);
            return 2;
        }
        fprintf(stderr, "Error: Cannot open chat file: %s\n", path);
        return 1;
    }
    fclose(f);

    /* Atomically set cursor (acquires chat lock) */
    int rc = chat_cursor_write(path, handle, (int)value);
    if (rc < 0) {
        fprintf(stderr, "Error: Failed to set cursor for '%s' in '%s'\n",
                handle, path);
        return 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Error: No command specified\n");
        fprintf(stderr, "Run 'nbs-chat help' for usage\n");
        return 4;
    }
    ASSERT_MSG(argc >= 2, "main: argc %d after validation", argc);

    const char *cmd = argv[1];

    /* Documented exit codes: 0 (success), 1 (error), 2 (not found),
     * 3 (timeout), 4 (invalid args). Validate postcondition. */
    int rc = -1;

    if (strcmp(cmd, "create") == 0) rc = cmd_create(argc, argv);
    else if (strcmp(cmd, "send") == 0) rc = cmd_send(argc, argv);
    else if (strcmp(cmd, "warn") == 0) rc = cmd_warn(argc, argv);
    else if (strcmp(cmd, "error") == 0) rc = cmd_error(argc, argv);
    else if (strcmp(cmd, "read") == 0) rc = cmd_read(argc, argv);
    else if (strcmp(cmd, "poll") == 0) rc = cmd_poll(argc, argv);
    else if (strcmp(cmd, "search") == 0) rc = cmd_search(argc, argv);
    else if (strcmp(cmd, "export") == 0) rc = cmd_export(argc, argv);
    else if (strcmp(cmd, "delete") == 0) rc = cmd_delete(argc, argv);
    else if (strcmp(cmd, "participants") == 0) rc = cmd_participants(argc, argv);
    else if (strcmp(cmd, "count") == 0) rc = cmd_count(argc, argv);
    else if (strcmp(cmd, "cursor-set") == 0) rc = cmd_cursor_set(argc, argv);
    else if (strcmp(cmd, "help") == 0) { print_usage(); return 0; }
    else {
        fprintf(stderr, "Error: Unknown command: %s\n", cmd);
        fprintf(stderr, "Run 'nbs-chat help' for usage\n");
        return 4;
    }

    ASSERT_MSG(rc >= 0 && rc <= 4,
               "main: cmd_%s returned undocumented exit code %d (expected 0-4)", cmd, rc);
    return rc;
}
