/*
 * main.c — nbs-scribe-log CLI
 *
 * Deterministic tool for appending decisions to the scribe log.
 * Replaces the LLM-constructed heredoc + nbs-bus publish sequence.
 *
 * Usage:
 *   nbs-scribe-log <log-file> <summary> [options]
 *
 * Exit codes:
 *   0  Success
 *   1  General error (I/O, lock)
 *   4  Invalid arguments
 */

#include "scribe_log.h"

#include <stdio.h>
#include <string.h>

static void print_usage(void)
{
    fprintf(stderr,
        "Usage: nbs-scribe-log <log-file> <summary> [options]\n"
        "\n"
        "Append a decision entry to the scribe log. Generates timestamp,\n"
        "formats the entry, appends to the log, and publishes a bus event.\n"
        "\n"
        "Required:\n"
        "  <log-file>                  Path to the scribe log (.md)\n"
        "  <summary>                   One-line decision summary\n"
        "  --participants=<a,b,c>      Comma-separated participant handles\n"
        "  --rationale=<text>          1-3 sentence rationale\n"
        "\n"
        "Optional:\n"
        "  --chat-ref=<file:~Lnnn>    Chat file and approximate line\n"
        "  --artefacts=<paths>         Commit hashes, file paths, or \"-\"\n"
        "  --risk-tags=<tags>          Comma-separated tags, or \"none\"\n"
        "  --status=<status>           decided|superseded|reversed (default: decided)\n"
        "  --supersedes=<D-timestamp>  Link to superseded decision\n"
        "  --bus-dir=<path>            Bus directory (default: .nbs/events/)\n"
        "\n"
        "Exit codes:\n"
        "  0  Success (decision ID printed to stdout)\n"
        "  1  General error (I/O, lock failure)\n"
        "  4  Invalid arguments\n"
        "\n"
        "Example:\n"
        "  nbs-scribe-log .nbs/scribe/live-log.md \"Use recursive descent parser\" \\\n"
        "    --participants=alex,claude \\\n"
        "    --chat-ref=live.chat:~L42 \\\n"
        "    --rationale=\"Grammar is LL(1), Pratt adds unnecessary complexity.\"\n"
    );
}

int main(int argc, char **argv)
{
    ASSERT_MSG(argv != NULL, "main: argv is NULL");

    if (argc < 2) {
        print_usage();
        return SCRIBE_EXIT_BAD_ARGS;
    }

    /* Check for --help anywhere */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return SCRIBE_EXIT_OK;
        }
    }

    if (argc < 3) {
        fprintf(stderr, "Error: requires <log-file> and <summary>\n");
        print_usage();
        return SCRIBE_EXIT_BAD_ARGS;
    }

    const char *log_path = argv[1];
    const char *summary = argv[2];

    if (log_path[0] == '\0') {
        fprintf(stderr, "Error: log file path is empty\n");
        return SCRIBE_EXIT_BAD_ARGS;
    }

    if (summary[0] == '\0') {
        fprintf(stderr, "Error: summary is empty\n");
        return SCRIBE_EXIT_BAD_ARGS;
    }

    /* Parse options */
    scribe_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.summary, sizeof(entry.summary), "%s", summary);

    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "--chat-ref=", 11) == 0) {
            snprintf(entry.chat_ref, sizeof(entry.chat_ref),
                     "%s", argv[i] + 11);
        } else if (strncmp(argv[i], "--participants=", 15) == 0) {
            snprintf(entry.participants, sizeof(entry.participants),
                     "%s", argv[i] + 15);
        } else if (strncmp(argv[i], "--artefacts=", 12) == 0) {
            snprintf(entry.artefacts, sizeof(entry.artefacts),
                     "%s", argv[i] + 12);
        } else if (strncmp(argv[i], "--risk-tags=", 12) == 0) {
            snprintf(entry.risk_tags, sizeof(entry.risk_tags),
                     "%s", argv[i] + 12);
        } else if (strncmp(argv[i], "--status=", 9) == 0) {
            const char *val = argv[i] + 9;
            if (strcmp(val, "decided") != 0 &&
                strcmp(val, "superseded") != 0 &&
                strcmp(val, "reversed") != 0 &&
                strcmp(val, "mitigated") != 0) {
                fprintf(stderr, "Error: --status must be "
                        "decided|superseded|reversed|mitigated, got '%s'\n",
                        val);
                return SCRIBE_EXIT_BAD_ARGS;
            }
            snprintf(entry.status, sizeof(entry.status), "%s", val);
        } else if (strncmp(argv[i], "--rationale=", 12) == 0) {
            snprintf(entry.rationale, sizeof(entry.rationale),
                     "%s", argv[i] + 12);
        } else if (strncmp(argv[i], "--supersedes=", 13) == 0) {
            snprintf(entry.supersedes, sizeof(entry.supersedes),
                     "%s", argv[i] + 13);
        } else if (strncmp(argv[i], "--bus-dir=", 10) == 0) {
            snprintf(entry.bus_dir, sizeof(entry.bus_dir),
                     "%s", argv[i] + 10);
        } else {
            fprintf(stderr, "Error: unknown option: %s\n", argv[i]);
            return SCRIBE_EXIT_BAD_ARGS;
        }
    }

    /* Validate required fields */
    if (entry.participants[0] == '\0') {
        fprintf(stderr, "Error: --participants is required\n");
        return SCRIBE_EXIT_BAD_ARGS;
    }

    if (entry.rationale[0] == '\0') {
        fprintf(stderr, "Error: --rationale is required\n");
        return SCRIBE_EXIT_BAD_ARGS;
    }

    return scribe_log_append(log_path, &entry);
}
