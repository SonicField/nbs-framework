/*
 * main.c — pty-session CLI entry point.
 *
 * Parses arguments and dispatches to the appropriate command handler
 * in session.c. All option parsing for individual commands is done here
 * to keep session.c focused on logic.
 *
 * Exit codes match the bash version:
 *   0 = success, 1 = error, 2 = not found, 3 = timeout, 4 = bad args
 */

#include "session.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * parse_int_option — Extract integer value from "--key=N" option.
 *
 * Returns the integer value, or default_val if parsing fails.
 */
static int parse_int_option(const char *arg, int default_val)
{
    ASSERT_MSG(arg != NULL, "parse_int_option: arg is NULL");

    const char *eq = strchr(arg, '=');
    if (!eq || eq[1] == '\0') {
        return default_val;
    }

    char *endptr;
    long val = strtol(eq + 1, &endptr, 10);
    if (*endptr != '\0' || val <= 0 || val > 100000) {
        return default_val;
    }

    return (int)val;
}

/*
 * join_args — Join argv[start..argc-1] into a single space-separated string.
 *
 * The caller must free the returned buffer.
 * Returns NULL on allocation failure.
 */
static char *join_args(int argc, char *argv[], int start)
{
    ASSERT_MSG(argv != NULL, "join_args: argv is NULL");
    ASSERT_MSG(start >= 0, "join_args: start is negative");

    if (start >= argc) {
        char *empty = malloc(1);
        if (empty) {
            empty[0] = '\0';
        }
        return empty;
    }

    /* Calculate total length */
    size_t total = 0;
    for (int i = start; i < argc; i++) {
        total += strlen(argv[i]);
        if (i < argc - 1) {
            total += 1; /* space */
        }
    }

    char *buf = malloc(total + 1);
    if (!buf) {
        return NULL;
    }

    size_t pos = 0;
    for (int i = start; i < argc; i++) {
        size_t len = strlen(argv[i]);
        memcpy(buf + pos, argv[i], len);
        pos += len;
        if (i < argc - 1) {
            buf[pos++] = ' ';
        }
    }
    buf[pos] = '\0';

    return buf;
}

/*
 * dispatch_create — Parse and dispatch "create <name> <command...>"
 */
static int dispatch_create(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Error: create requires <name> and <command>\n");
        fprintf(stderr, "Usage: pty-session create <name> <command>\n");
        return EXIT_BAD_ARGS;
    }

    const char *name = argv[2];
    char *command = join_args(argc, argv, 3);
    if (!command) {
        fprintf(stderr, "Error: malloc failed\n");
        return EXIT_ERROR;
    }

    int rc = cmd_create(name, command);
    free(command);
    return rc;
}

/*
 * dispatch_send — Parse and dispatch "send <name> [--no-enter] <text...>"
 */
static int dispatch_send(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Error: send requires <name> and <text>\n");
        return EXIT_BAD_ARGS;
    }

    const char *name = argv[2];
    int no_enter = 0;
    int text_start = 3;

    /* Check for --no-enter flag */
    if (argc > 3 && strcmp(argv[3], "--no-enter") == 0) {
        no_enter = 1;
        text_start = 4;
    }

    if (text_start >= argc) {
        fprintf(stderr, "Error: send requires <text>\n");
        return EXIT_BAD_ARGS;
    }

    char *text = join_args(argc, argv, text_start);
    if (!text) {
        fprintf(stderr, "Error: malloc failed\n");
        return EXIT_ERROR;
    }

    int rc = cmd_send(name, text, no_enter);
    free(text);
    return rc;
}

/*
 * dispatch_read — Parse and dispatch "read <name> [options]"
 */
static int dispatch_read(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Error: read requires <name>\n");
        return EXIT_BAD_ARGS;
    }

    const char *name = argv[2];
    int scrollback = DEFAULT_SCROLLBACK;
    int wait_mode = 0;
    int timeout = DEFAULT_READ_TIMEOUT;

    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "--scrollback=", 13) == 0 ||
            strncmp(argv[i], "--last=", 7) == 0) {
            scrollback = parse_int_option(argv[i], scrollback);
        } else if (strcmp(argv[i], "--wait") == 0) {
            wait_mode = 1;
        } else if (strncmp(argv[i], "--timeout=", 10) == 0) {
            timeout = parse_int_option(argv[i], timeout);
        }
    }

    return cmd_read(name, scrollback, wait_mode, timeout);
}

/*
 * dispatch_wait — Parse and dispatch "wait <name> <pattern> [--timeout=N]"
 */
static int dispatch_wait(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Error: wait requires <name> and <pattern>\n");
        return EXIT_BAD_ARGS;
    }

    const char *name = argv[2];
    const char *pattern = argv[3];
    int timeout = DEFAULT_WAIT_TIMEOUT;

    for (int i = 4; i < argc; i++) {
        if (strncmp(argv[i], "--timeout=", 10) == 0) {
            timeout = parse_int_option(argv[i], timeout);
        }
    }

    return cmd_wait(name, pattern, timeout);
}

/*
 * dispatch_kill — Parse and dispatch "kill <name>"
 */
static int dispatch_kill(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Error: kill requires <name>\n");
        return EXIT_BAD_ARGS;
    }

    return cmd_kill(argv[2]);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        return cmd_help();
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "create") == 0) {
        return dispatch_create(argc, argv);
    } else if (strcmp(cmd, "send") == 0) {
        return dispatch_send(argc, argv);
    } else if (strcmp(cmd, "read") == 0) {
        return dispatch_read(argc, argv);
    } else if (strcmp(cmd, "wait") == 0) {
        return dispatch_wait(argc, argv);
    } else if (strcmp(cmd, "kill") == 0) {
        return dispatch_kill(argc, argv);
    } else if (strcmp(cmd, "list") == 0) {
        return cmd_list();
    } else if (strcmp(cmd, "help") == 0 ||
               strcmp(cmd, "--help") == 0 ||
               strcmp(cmd, "-h") == 0) {
        return cmd_help();
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        fprintf(stderr, "Run 'pty-session help' for usage\n");
        return EXIT_BAD_ARGS;
    }
}
