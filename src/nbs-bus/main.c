/*
 * main.c — NBS Bus command-line interface
 *
 * Subcommand dispatch via strcmp chain, consistent with nbs-chat and nbs-hub.
 *
 * Exit codes:
 *   0  Success
 *   1  General error
 *   2  Events directory not found
 *   3  Event file not found
 *   4  Invalid arguments
 *   5  Deduplication (event dropped)
 */

#include "bus.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

/* Default retention limit: 16 MB */
#define DEFAULT_MAX_BYTES (16LL * 1024 * 1024)

static void print_usage(void)
{
    fprintf(stderr,
        "Usage: nbs-bus <command> [args...]\n"
        "\n"
        "Commands:\n"
        "  publish <dir> <source> <type> <priority> [payload]\n"
        "      Write an event file to the queue.\n"
        "\n"
        "  check <dir> [--handle=<name>]\n"
        "      List pending events, highest priority first.\n"
        "\n"
        "  read <dir> <event-file>\n"
        "      Read a single event file.\n"
        "\n"
        "  ack <dir> <event-file>\n"
        "      Acknowledge an event (move to processed/).\n"
        "\n"
        "  ack-all <dir> [--handle=<name>]\n"
        "      Acknowledge all pending events.\n"
        "\n"
        "  prune <dir> [--max-bytes=N]\n"
        "      Delete oldest processed events when size limit exceeded.\n"
        "      Default limit: 16 MB (16777216 bytes).\n"
        "\n"
        "  status <dir>\n"
        "      Summary: pending count by priority, processed count.\n"
        "\n"
        "  help\n"
        "      Print this usage message.\n"
        "\n"
        "Exit codes:\n"
        "  0  Success\n"
        "  1  General error\n"
        "  2  Events directory not found\n"
        "  3  Event file not found\n"
        "  4  Invalid arguments\n"
        "  5  Deduplication (event dropped)\n"
    );
}

/* Parse --handle=<name> from argv. Returns handle string or NULL. */
static const char *parse_handle_opt(int argc, char **argv, int start)
{
    for (int i = start; i < argc; i++) {
        if (strncmp(argv[i], "--handle=", 9) == 0) {
            const char *h = argv[i] + 9;
            if (h[0] == '\0') return NULL;
            return h;
        }
    }
    return NULL;
}

/* Parse --max-bytes=N from argv. Returns value or default. */
static long long parse_max_bytes_opt(int argc, char **argv, int start)
{
    for (int i = start; i < argc; i++) {
        if (strncmp(argv[i], "--max-bytes=", 12) == 0) {
            const char *s = argv[i] + 12;
            char *endp;
            errno = 0;
            long long val = strtoll(s, &endp, 10);
            if (errno != 0 || *endp != '\0' || val <= 0) {
                fprintf(stderr, "Error: invalid --max-bytes value: %s\n", s);
                return -1;
            }
            return val;
        }
    }
    return DEFAULT_MAX_BYTES;
}

/* Verify events directory exists, print appropriate error if not. */
static int verify_events_dir(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) != 0) {
        fprintf(stderr, "Error: events directory not found: %s\n", dir);
        return BUS_EXIT_DIR_NOT_FOUND;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: not a directory: %s\n", dir);
        return BUS_EXIT_DIR_NOT_FOUND;
    }
    return 0;
}

/* --- Command handlers --- */

static int cmd_publish(int argc, char **argv)
{
    /* nbs-bus publish <dir> <source> <type> <priority> [payload] */
    if (argc < 6) {
        fprintf(stderr, "Usage: nbs-bus publish <dir> <source> <type> <priority> [payload]\n");
        return BUS_EXIT_BAD_ARGS;
    }

    const char *dir = argv[2];
    const char *source = argv[3];
    const char *type = argv[4];
    const char *priority_str = argv[5];
    const char *payload = (argc > 6) ? argv[6] : NULL;

    int rc = verify_events_dir(dir);
    if (rc != 0) return rc;

    int priority = bus_priority_from_str(priority_str);
    if (priority < 0) {
        fprintf(stderr, "Error: invalid priority '%s' (use: critical, high, normal, low)\n",
                priority_str);
        return BUS_EXIT_BAD_ARGS;
    }

    if (bus_publish(dir, source, type, priority, payload) != 0)
        return BUS_EXIT_ERROR;

    return BUS_EXIT_OK;
}

static int cmd_check(int argc, char **argv)
{
    /* nbs-bus check <dir> [--handle=<name>] */
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-bus check <dir> [--handle=<name>]\n");
        return BUS_EXIT_BAD_ARGS;
    }

    const char *dir = argv[2];
    int rc = verify_events_dir(dir);
    if (rc != 0) return rc;

    const char *handle = parse_handle_opt(argc, argv, 3);

    if (bus_check(dir, handle) != 0)
        return BUS_EXIT_ERROR;

    return BUS_EXIT_OK;
}

static int cmd_read(int argc, char **argv)
{
    /* nbs-bus read <dir> <event-file> */
    if (argc < 4) {
        fprintf(stderr, "Usage: nbs-bus read <dir> <event-file>\n");
        return BUS_EXIT_BAD_ARGS;
    }

    const char *dir = argv[2];
    const char *event_file = argv[3];

    int rc = verify_events_dir(dir);
    if (rc != 0) return rc;

    if (bus_read(dir, event_file) != 0)
        return BUS_EXIT_NOT_FOUND;

    return BUS_EXIT_OK;
}

static int cmd_ack(int argc, char **argv)
{
    /* nbs-bus ack <dir> <event-file> */
    if (argc < 4) {
        fprintf(stderr, "Usage: nbs-bus ack <dir> <event-file>\n");
        return BUS_EXIT_BAD_ARGS;
    }

    const char *dir = argv[2];
    const char *event_file = argv[3];

    int rc = verify_events_dir(dir);
    if (rc != 0) return rc;

    if (bus_ack(dir, event_file) != 0)
        return BUS_EXIT_NOT_FOUND;

    return BUS_EXIT_OK;
}

static int cmd_ack_all(int argc, char **argv)
{
    /* nbs-bus ack-all <dir> [--handle=<name>] */
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-bus ack-all <dir> [--handle=<name>]\n");
        return BUS_EXIT_BAD_ARGS;
    }

    const char *dir = argv[2];
    int rc = verify_events_dir(dir);
    if (rc != 0) return rc;

    const char *handle = parse_handle_opt(argc, argv, 3);

    if (bus_ack_all(dir, handle) != 0)
        return BUS_EXIT_ERROR;

    return BUS_EXIT_OK;
}

static int cmd_prune(int argc, char **argv)
{
    /* nbs-bus prune <dir> [--max-bytes=N] */
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-bus prune <dir> [--max-bytes=N]\n");
        return BUS_EXIT_BAD_ARGS;
    }

    const char *dir = argv[2];
    int rc = verify_events_dir(dir);
    if (rc != 0) return rc;

    long long max_bytes = parse_max_bytes_opt(argc, argv, 3);
    if (max_bytes < 0)
        return BUS_EXIT_BAD_ARGS;

    if (bus_prune(dir, max_bytes) != 0)
        return BUS_EXIT_ERROR;

    return BUS_EXIT_OK;
}

static int cmd_status(int argc, char **argv)
{
    /* nbs-bus status <dir> */
    if (argc < 3) {
        fprintf(stderr, "Usage: nbs-bus status <dir>\n");
        return BUS_EXIT_BAD_ARGS;
    }

    const char *dir = argv[2];
    int rc = verify_events_dir(dir);
    if (rc != 0) return rc;

    if (bus_status(dir) != 0)
        return BUS_EXIT_ERROR;

    return BUS_EXIT_OK;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return BUS_EXIT_BAD_ARGS;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "publish") == 0)  return cmd_publish(argc, argv);
    if (strcmp(cmd, "check") == 0)    return cmd_check(argc, argv);
    if (strcmp(cmd, "read") == 0)     return cmd_read(argc, argv);
    if (strcmp(cmd, "ack") == 0)      return cmd_ack(argc, argv);
    if (strcmp(cmd, "ack-all") == 0)  return cmd_ack_all(argc, argv);
    if (strcmp(cmd, "prune") == 0)    return cmd_prune(argc, argv);
    if (strcmp(cmd, "status") == 0)   return cmd_status(argc, argv);
    if (strcmp(cmd, "help") == 0)     { print_usage(); return BUS_EXIT_OK; }

    fprintf(stderr, "Error: unknown command: %s\n", cmd);
    print_usage();
    return BUS_EXIT_BAD_ARGS;
}
