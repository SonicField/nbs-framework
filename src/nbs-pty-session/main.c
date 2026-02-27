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
#include <errno.h>
#include <limits.h>
#include <ctype.h>

/*
 * MAX_OPTION_VALUE — Upper bound for integer option values.
 *
 * Rationale: This is the ceiling for all --timeout and --scrollback
 * values. 100000 seconds (~27 hours) is a generous upper bound for any
 * interactive session timeout. Scrollback of 100000 lines is similarly
 * extreme but not unreasonable for long-running sessions.
 */
#define MAX_OPTION_VALUE 100000

/*
 * MAX_DISPLAY_LEN — Maximum length for user-provided strings in error
 * messages. Prevents unbounded output to stderr (Violation M6).
 */
#define MAX_DISPLAY_LEN 256

/*
 * sanitise_for_display — Replace non-printable characters with '?'.
 *
 * Preconditions:
 *   - input != NULL
 *   - buf != NULL, bufsize > 0
 *
 * Postconditions:
 *   - buf contains NUL-terminated string with only printable ASCII
 *   - Non-printable characters (including ANSI escapes) replaced with '?'
 *   - Output truncated to bufsize-1 characters
 *
 * Addresses Violation M5 (SECURITY): terminal escape injection via
 * unsanitised user input in error messages.
 */
static void sanitise_for_display(const char *input, char *buf, size_t bufsize)
{
    ASSERT_MSG(input != NULL, "sanitise_for_display: input is NULL");
    ASSERT_MSG(buf != NULL, "sanitise_for_display: buf is NULL");
    ASSERT_MSG(bufsize > 0, "sanitise_for_display: bufsize is 0");

    size_t i;
    size_t max = bufsize - 1;
    for (i = 0; i < max && input[i] != '\0'; i++) {
        /* Accept printable ASCII only (0x20-0x7E) */
        if (input[i] >= 0x20 && input[i] <= 0x7E) {
            buf[i] = input[i];
        } else {
            buf[i] = '?';
        }
    }
    buf[i] = '\0';
}

/*
 * parse_int_option — Extract integer value from "--key=N" option.
 *
 * Preconditions:
 *   - arg != NULL
 *
 * Postconditions:
 *   - If no '=' or empty after '=': returns default_val (not an error;
 *     the option was present but the value portion was absent)
 *   - If value is a valid integer in 1..MAX_OPTION_VALUE: returns value
 *   - If value is invalid (non-numeric, out of range, overflow): returns -1
 *
 * Returns -1 as error sentinel. Callers must check for -1 and return
 * EXIT_BAD_ARGS. (Violation M1: no longer silently returns default.)
 */
static int parse_int_option(const char *arg, int default_val)
{
    ASSERT_MSG(arg != NULL, "parse_int_option: arg is NULL");

    const char *eq = strchr(arg, '=');
    if (!eq || eq[1] == '\0') {
        return default_val;
    }

    /* Violation M7 fix: check errno for strtol overflow */
    errno = 0;
    char *endptr;
    long val = strtol(eq + 1, &endptr, 10);

    if (errno == ERANGE) {
        fprintf(stderr, "Error: numeric overflow in '%s': "
                "value must be an integer in 1..%d\n",
                arg, MAX_OPTION_VALUE);
        return -1;
    }

    if (*endptr != '\0') {
        fprintf(stderr, "Error: non-numeric value in '%s': "
                "must be an integer in 1..%d\n",
                arg, MAX_OPTION_VALUE);
        return -1;
    }

    if (val <= 0 || val > MAX_OPTION_VALUE) {
        /* Violation M9 fix: message includes valid range */
        fprintf(stderr, "Error: value out of range in '%s': "
                "must be an integer in 1..%d\n",
                arg, MAX_OPTION_VALUE);
        return -1;
    }

    return (int)val;
}

/*
 * join_args — Join argv[start..argc-1] into a single space-separated string.
 *
 * The caller must free the returned buffer.
 * Returns NULL on allocation failure.
 *
 * Only used by dispatch_create and dispatch_send (excluded in TEST_BUILD).
 */
#ifndef TEST_BUILD
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
        ASSERT_MSG(argv[i] != NULL, "join_args: argv[%d] is NULL", i);
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

    ASSERT_MSG(pos == total, "join_args: length mismatch, expected %zu got %zu", total, pos);

    return buf;
}
#endif /* TEST_BUILD — join_args */

/*
 * The following dispatch functions (create, send, kill) are only used
 * by main(), which is excluded in TEST_BUILD. Guard them to avoid
 * unused-function warnings under -Werror.
 */
#ifndef TEST_BUILD

/*
 * dispatch_create — Parse and dispatch "create <name> <command...>"
 */
static int dispatch_create(int argc, char *argv[])
{
    ASSERT_MSG(argv != NULL, "dispatch_create: argv is NULL");
    ASSERT_MSG(argc >= 1, "dispatch_create: argc is non-positive");

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
    ASSERT_MSG(argv != NULL, "dispatch_send: argv is NULL");
    ASSERT_MSG(argc >= 1, "dispatch_send: argc is non-positive");

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

#endif /* TEST_BUILD — dispatch_create, dispatch_send */

/*
 * dispatch_read — Parse and dispatch "read <name> [options]"
 *
 * Violation M3 fix: unrecognised options now rejected with EXIT_BAD_ARGS.
 */
static int dispatch_read(int argc, char *argv[])
{
    ASSERT_MSG(argv != NULL, "dispatch_read: argv is NULL");
    ASSERT_MSG(argc >= 1, "dispatch_read: argc is non-positive");

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
            if (scrollback == -1) {
                return EXIT_BAD_ARGS;
            }
        } else if (strcmp(argv[i], "--wait") == 0) {
            wait_mode = 1;
        } else if (strncmp(argv[i], "--timeout=", 10) == 0) {
            timeout = parse_int_option(argv[i], timeout);
            if (timeout == -1) {
                return EXIT_BAD_ARGS;
            }
        } else {
            /* Violation M3 fix: reject unrecognised options */
            char safe_opt[MAX_DISPLAY_LEN];
            sanitise_for_display(argv[i], safe_opt, sizeof(safe_opt));
            fprintf(stderr, "Error: unrecognised option '%s'\n", safe_opt);
            fprintf(stderr, "Valid options: --scrollback=N, --last=N, --wait, --timeout=N\n");
            return EXIT_BAD_ARGS;
        }
    }

    return cmd_read(name, scrollback, wait_mode, timeout);
}

/*
 * dispatch_wait — Parse and dispatch "wait <name> <pattern> [--timeout=N]"
 *
 * Violation M4 fix: unrecognised options now rejected with EXIT_BAD_ARGS.
 */
static int dispatch_wait(int argc, char *argv[])
{
    ASSERT_MSG(argv != NULL, "dispatch_wait: argv is NULL");
    ASSERT_MSG(argc >= 1, "dispatch_wait: argc is non-positive");

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
            if (timeout == -1) {
                return EXIT_BAD_ARGS;
            }
        } else {
            /* Violation M4 fix: reject unrecognised options */
            char safe_opt[MAX_DISPLAY_LEN];
            sanitise_for_display(argv[i], safe_opt, sizeof(safe_opt));
            fprintf(stderr, "Error: unrecognised option '%s'\n", safe_opt);
            fprintf(stderr, "Valid options: --timeout=N\n");
            return EXIT_BAD_ARGS;
        }
    }

    return cmd_wait(name, pattern, timeout);
}

#ifndef TEST_BUILD
/*
 * dispatch_kill — Parse and dispatch "kill <name>"
 */
static int dispatch_kill(int argc, char *argv[])
{
    ASSERT_MSG(argv != NULL, "dispatch_kill: argv is NULL");
    ASSERT_MSG(argc >= 1, "dispatch_kill: argc is non-positive");

    if (argc < 3) {
        fprintf(stderr, "Error: kill requires <name>\n");
        return EXIT_BAD_ARGS;
    }

    return cmd_kill(argv[2]);
}
#endif /* TEST_BUILD — dispatch_kill */

#ifndef TEST_BUILD
int main(int argc, char *argv[])
{
    ASSERT_MSG(argv != NULL, "main: argv is NULL");

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
        /*
         * Violation M5 fix: sanitise cmd before printing to prevent
         * terminal escape injection.
         * Violation M6 fix: truncate to MAX_DISPLAY_LEN.
         */
        char safe_cmd[MAX_DISPLAY_LEN];
        sanitise_for_display(cmd, safe_cmd, sizeof(safe_cmd));
        fprintf(stderr, "Unknown command: %s\n", safe_cmd);
        fprintf(stderr, "Run 'pty-session help' for usage\n");
        return EXIT_BAD_ARGS;
    }
}
#endif /* TEST_BUILD */

/* ── Test-visible wrappers ────────────────────────────────────────── */

#ifdef TEST_BUILD

int test_parse_int_option(const char *arg, int default_val)
{
    return parse_int_option(arg, default_val);
}

void test_sanitise_for_display(const char *input, char *buf, size_t bufsize)
{
    sanitise_for_display(input, buf, bufsize);
}

int test_dispatch_read(int argc, char *argv[])
{
    return dispatch_read(argc, argv);
}

int test_dispatch_wait(int argc, char *argv[])
{
    return dispatch_wait(argc, argv);
}

#endif /* TEST_BUILD */
