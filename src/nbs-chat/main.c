/*
 * main.c — nbs-chat CLI tool
 *
 * Usage: nbs-chat <command> [args...]
 *
 * Commands:
 *   create <file>                    Create new chat file
 *   send <file> <handle> <message>   Send a message
 *   read <file> [options]            Read messages
 *   poll <file> <handle> [options]   Wait for new message
 *   search <file> <pattern> [opts]   Search message history
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
#include <assert.h>
#include <ctype.h>
#include <errno.h>
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
    printf("  read <file> [options]            Read messages\n");
    printf("  poll <file> <handle> [options]   Wait for new message\n");
    printf("  search <file> <pattern> [opts]   Search message history\n");
    printf("  participants <file>              List participants and counts\n");
    printf("  help                             Show this help\n\n");
    printf("Read options:\n");
    printf("  --last=N           Show only the last N messages\n");
    printf("  --since=<handle>   Show messages after last message from <handle>\n");
    printf("  --unread=<handle>  Show messages after read cursor for <handle>\n");
    printf("                     Auto-advances cursor after displaying\n\n");
    printf("Search options:\n");
    printf("  --handle=<name>  Only search messages from this handle\n\n");
    printf("Poll options:\n");
    printf("  --timeout=N      Timeout in seconds (default: 10)\n\n");
    printf("Exit codes:\n");
    printf("  0 - Success\n");
    printf("  1 - General error\n");
    printf("  2 - File not found / already exists\n");
    printf("  3 - Timeout (poll only)\n");
    printf("  4 - Invalid arguments\n");
}

static int cmd_create(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-chat create <file>\n");
        return 4;
    }

    const char *path = argv[2];

    /* Precondition: path validated from argv */
    ASSERT_MSG(path != NULL, "cmd_create: path argument is NULL after argv extraction — this indicates an internal argument parsing error");

    /* Resolve to absolute path */
    char abs_path[MAX_PATH_LEN * 2];
    if (path[0] != '/') {
        char cwd[MAX_PATH_LEN];
        char *cwdp = getcwd(cwd, sizeof(cwd));
        ASSERT_MSG(cwdp != NULL, "cmd_create: getcwd failed: %s — cannot resolve relative path '%s'",
                   strerror(errno), path);
        snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd, path);
        path = abs_path;
    }

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

    const char *path = argv[2];
    const char *handle = argv[3];
    const char *message = argv[4];

    /* Preconditions: args validated from argv */
    ASSERT_MSG(path != NULL, "cmd_send: path argument is NULL after argv extraction — this indicates an internal argument parsing error");
    ASSERT_MSG(handle != NULL, "cmd_send: handle argument is NULL after argv extraction — this indicates an internal argument parsing error");
    ASSERT_MSG(message != NULL, "cmd_send: message argument is NULL after argv extraction — this indicates an internal argument parsing error");

    /* Check file exists */
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: Chat file not found: %s\n", path);
        return 2;
    }
    fclose(f);

    int result = chat_send(path, handle, message);
    if (result < 0) {
        fprintf(stderr, "Error: Failed to send message\n");
        return 1;
    }

    /* Publish bus events (if bus directory exists).
     * Bus failure is non-fatal — chat send already succeeded. */
    bus_bridge_after_send(path, handle, message);

    return 0;
}

static int cmd_read(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-chat read <file> [--last=N] [--since=<handle>] [--unread=<handle>]\n");
        return 4;
    }

    const char *path = argv[2];

    /* Precondition: path validated from argv */
    ASSERT_MSG(path != NULL, "cmd_read: path argument is NULL after argv extraction — this indicates an internal argument parsing error");

    int last_n = -1;
    const char *since_handle = NULL;
    const char *unread_handle = NULL;

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
        } else if (strncmp(argv[i], "--since=", 8) == 0) {
            since_handle = argv[i] + 8;
        } else if (strncmp(argv[i], "--unread=", 9) == 0) {
            unread_handle = argv[i] + 9;
        } else {
            fprintf(stderr, "Warning: Unknown option: %s\n", argv[i]);
        }
    }

    /* Check file exists */
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: Chat file not found: %s\n", path);
        return 2;
    }
    fclose(f);

    chat_state_t state;
    if (chat_read(path, &state) < 0) {
        fprintf(stderr, "Error: Failed to read chat file\n");
        return 1;
    }

    int start = 0;
    int end = state.message_count;

    /* Apply --unread filter (takes precedence over --since) */
    if (unread_handle) {
        int cursor = chat_cursor_read(path, unread_handle);
        /* cursor is last-read index; show messages after it */
        start = cursor + 1;  /* -1 + 1 = 0 if no cursor exists (show all) */
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

    /* Apply --last filter */
    if (last_n >= 0 && end - start > last_n) {
        start = end - last_n;
    }

    /* Print messages */
    for (int i = start; i < end; i++) {
        printf("%s: %s\n", state.messages[i].handle, state.messages[i].content);
    }

    /* Advance read cursor after displaying */
    if (unread_handle && end > 0) {
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

    const char *path = argv[2];
    const char *handle = argv[3];

    /* Preconditions: args validated from argv */
    ASSERT_MSG(path != NULL, "cmd_poll: path argument is NULL after argv extraction — this indicates an internal argument parsing error");
    ASSERT_MSG(handle != NULL, "cmd_poll: handle argument is NULL after argv extraction — this indicates an internal argument parsing error");

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
        } else {
            fprintf(stderr, "Warning: Unknown option: %s\n", argv[i]);
        }
    }

    /* Check file exists */
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: Chat file not found: %s\n", path);
        return 2;
    }
    fclose(f);

    int result = chat_poll(path, handle, timeout);
    if (result == 3) return 3; /* Timeout */
    if (result < 0) {
        fprintf(stderr, "Error: Poll failed\n");
        return 1;
    }

    /* Print new messages */
    chat_state_t state;
    if (chat_read(path, &state) == 0) {
        /* Print the last message from someone other than the polling handle */
        for (int i = state.message_count - 1; i >= 0; i--) {
            if (strcmp(state.messages[i].handle, handle) != 0) {
                printf("%s: %s\n", state.messages[i].handle,
                       state.messages[i].content);
                break;
            }
        }
        chat_state_free(&state);
    }

    return 0;
}

static int cmd_participants(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-chat participants <file>\n");
        return 4;
    }

    const char *path = argv[2];

    /* Precondition: path validated from argv */
    ASSERT_MSG(path != NULL, "cmd_participants: path argument is NULL after argv extraction — this indicates an internal argument parsing error");

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: Chat file not found: %s\n", path);
        return 2;
    }
    fclose(f);

    chat_state_t state;
    if (chat_read(path, &state) < 0) {
        fprintf(stderr, "Error: Failed to read chat file\n");
        return 1;
    }

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
        fprintf(stderr, "Usage: nbs-chat search <file> <pattern> [--handle=<name>]\n");
        return 4;
    }

    const char *path = argv[2];
    const char *pattern = argv[3];
    const char *filter_handle = NULL;

    /* Preconditions: args validated from argv */
    ASSERT_MSG(path != NULL, "cmd_search: path argument is NULL");
    ASSERT_MSG(pattern != NULL, "cmd_search: pattern argument is NULL");

    /* Parse options */
    for (int i = 4; i < argc; i++) {
        if (strncmp(argv[i], "--handle=", 9) == 0) {
            filter_handle = argv[i] + 9;
        } else {
            fprintf(stderr, "Warning: Unknown option: %s\n", argv[i]);
        }
    }

    /* Check file exists */
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: Chat file not found: %s\n", path);
        return 2;
    }
    fclose(f);

    chat_state_t state;
    if (chat_read(path, &state) < 0) {
        fprintf(stderr, "Error: Failed to read chat file\n");
        return 1;
    }

    int match_count = 0;
    for (int i = 0; i < state.message_count; i++) {
        /* Apply handle filter if specified */
        if (filter_handle && strcmp(state.messages[i].handle, filter_handle) != 0) {
            continue;
        }

        /* Case-insensitive search in message content */
        if (strcasestr_portable(state.messages[i].content, pattern) != NULL) {
            printf("[%d] %s: %s\n", i, state.messages[i].handle,
                   state.messages[i].content);
            match_count++;
        }
    }

    if (match_count == 0) {
        /* No matches — exit code 0 (not an error, just no results) */
        printf("No matches found.\n");
    }

    chat_state_free(&state);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Error: No command specified\n");
        fprintf(stderr, "Run 'nbs-chat help' for usage\n");
        return 4;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "create") == 0) return cmd_create(argc, argv);
    if (strcmp(cmd, "send") == 0) return cmd_send(argc, argv);
    if (strcmp(cmd, "read") == 0) return cmd_read(argc, argv);
    if (strcmp(cmd, "poll") == 0) return cmd_poll(argc, argv);
    if (strcmp(cmd, "search") == 0) return cmd_search(argc, argv);
    if (strcmp(cmd, "participants") == 0) return cmd_participants(argc, argv);
    if (strcmp(cmd, "help") == 0) { print_usage(); return 0; }

    fprintf(stderr, "Error: Unknown command: %s\n", cmd);
    fprintf(stderr, "Run 'nbs-chat help' for usage\n");
    return 4;
}
