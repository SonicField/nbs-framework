/*
 * main.c — NBS Bus command-line interface
 *
 * Subcommand dispatch via strcmp chain, consistent with nbs-chat.
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

static void print_usage(void)
{
    fprintf(stderr,
        "Usage: nbs-bus <command> [args...]\n"
        "\n"
        "Commands:\n"
        "  publish <dir> <source> <type> <priority> [payload] [--dedup-window=N]\n"
        "      Write an event file to the queue.\n"
        "      --dedup-window=N: drop if same source:type exists within N seconds.\n"
        "                        Default: 0 (disabled), or from config.yaml.\n"
        "                        Exit code 5 when deduplicated.\n"
        "\n"
        "  check <dir> [--handle=<name>]\n"
        "      List pending events, highest priority first.\n"
        "      Output: [priority] filename (age)\n"
        "      --handle=<name>: show only events from this source.\n"
        "\n"
        "  read <dir> <event-file>\n"
        "      Read a single event file.\n"
        "\n"
        "  ack <dir> <event-file>\n"
        "      Acknowledge an event (move to processed/).\n"
        "\n"
        "  ack-all <dir> [--handle=<name>]\n"
        "      Acknowledge all pending events.\n"
        "      --handle=<name>: ack only events from this source.\n"
        "\n"
        "  prune <dir> [--max-bytes=N]\n"
        "      Delete oldest processed events when size limit exceeded.\n"
        "      Default: 16 MB, or from config.yaml retention-max-bytes.\n"
        "\n"
        "  status <dir>\n"
        "      Summary: pending count by priority, processed count.\n"
        "      Warns about stale events if ack-timeout set in config.yaml.\n"
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

/* Parse --max-bytes=N from argv. Returns value, or cfg_default if not specified. */
static long long parse_max_bytes_opt(int argc, char **argv, int start,
                                     long long cfg_default)
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
    return cfg_default;
}

/* Parse --dedup-window=<seconds> from argv. Returns microseconds, or
 * cfg_default_us if not specified. cfg_default_us is already in microseconds. */
static long long parse_dedup_window_opt(int argc, char **argv, int start,
                                        long long cfg_default_us)
{
    for (int i = start; i < argc; i++) {
        if (strncmp(argv[i], "--dedup-window=", 15) == 0) {
            const char *s = argv[i] + 15;
            char *endp;
            errno = 0;
            long long val = strtoll(s, &endp, 10);
            if (errno != 0 || *endp != '\0' || val < 0) {
                fprintf(stderr, "Error: invalid --dedup-window value: %s\n", s);
                return -1;
            }
            return val * 1000000LL; /* seconds to microseconds */
        }
    }
    return cfg_default_us;
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
    /* nbs-bus publish <dir> <source> <type> <priority> [payload] [--dedup-window=N] */
    if (argc < 6) {
        fprintf(stderr, "Usage: nbs-bus publish <dir> <source> <type> <priority> [payload] [--dedup-window=N]\n");
        return BUS_EXIT_BAD_ARGS;
    }

    const char *dir = argv[2];
    const char *source = argv[3];
    const char *type = argv[4];
    const char *priority_str = argv[5];

    /* Payload is the first positional arg after priority that doesn't start with -- */
    const char *payload = NULL;
    if (argc > 6 && strncmp(argv[6], "--", 2) != 0)
        payload = argv[6];

    int rc = verify_events_dir(dir);
    if (rc != 0) return rc;

    int priority = bus_priority_from_str(priority_str);
    if (priority < 0) {
        fprintf(stderr, "Error: invalid priority '%s' (use: critical, high, normal, low)\n",
                priority_str);
        return BUS_EXIT_BAD_ARGS;
    }

    /* Load config for defaults; CLI args override */
    bus_config_t cfg;
    bus_load_config(dir, &cfg);

    long long dedup_window_us = parse_dedup_window_opt(argc, argv, 6,
                                                        cfg.dedup_window_s * 1000000LL);
    if (dedup_window_us < 0)
        return BUS_EXIT_BAD_ARGS;

    if (dedup_window_us > 0) {
        rc = bus_publish_dedup(dir, source, type, priority, payload, dedup_window_us);
        if (rc == BUS_EXIT_DEDUP)
            return BUS_EXIT_DEDUP;
        if (rc != 0)
            return BUS_EXIT_ERROR;
        return BUS_EXIT_OK;
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

    /* Load config for defaults; CLI args override */
    bus_config_t cfg;
    bus_load_config(dir, &cfg);

    long long max_bytes = parse_max_bytes_opt(argc, argv, 3,
                                              cfg.retention_max_bytes);
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
