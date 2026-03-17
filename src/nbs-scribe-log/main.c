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

/*
 * check_snprintf — Validate snprintf result for truncation or error.
 *
 * Returns 0 on success, prints error and returns SCRIBE_EXIT_BAD_ARGS
 * if truncation occurred or snprintf returned an error.
 */
static int check_snprintf(int ret, size_t bufsize, const char *field_name)
{
    ASSERT_MSG(field_name != NULL,
               "check_snprintf: field_name is NULL");
    ASSERT_MSG(bufsize > 0,
               "check_snprintf: bufsize is 0 for field '%s'", field_name);

    if (ret < 0) {
        fprintf(stderr, "Error: encoding error writing %s\n", field_name);
        return SCRIBE_EXIT_BAD_ARGS;
    }
    if ((size_t)ret >= bufsize) {
        fprintf(stderr, "Error: %s exceeds %zu characters\n",
                field_name, bufsize - 1);
        return SCRIBE_EXIT_BAD_ARGS;
    }
    return 0;
}

/*
 * check_no_newline — Reject field values containing newlines.
 *
 * Newlines in single-line Markdown fields would allow injection of
 * fake decision entries into the log (SECURITY: Markdown injection).
 */
static int check_no_newline(const char *value, const char *field_name)
{
    ASSERT_MSG(field_name != NULL,
               "check_no_newline: field_name is NULL");

    if (value == NULL) return 0;
    if (strchr(value, '\n') != NULL) {
        fprintf(stderr, "Error: %s contains newline (injection risk)\n",
                field_name);
        return SCRIBE_EXIT_BAD_ARGS;
    }
    return 0;
}

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
        "  --status=<status>           decided|superseded|reversed|mitigated (default: decided)\n"
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
        ASSERT_MSG(argv[i] != NULL, "main: argv[%d] is NULL", i);
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

    /* Precondition assertions — these are guaranteed by argc >= 3, but
     * the engineering standard requires explicit executable specifications. */
    ASSERT_MSG(log_path != NULL, "main: log_path is NULL");
    ASSERT_MSG(summary != NULL, "main: summary is NULL");

    if (log_path[0] == '\0') {
        fprintf(stderr, "Error: log file path is empty\n");
        return SCRIBE_EXIT_BAD_ARGS;
    }

    /* SECURITY: Validate path length before it reaches lock_acquire/snprintf */
    if (strlen(log_path) >= SCRIBE_MAX_PATH) {
        fprintf(stderr, "Error: log file path exceeds %d characters\n",
                SCRIBE_MAX_PATH - 1);
        return SCRIBE_EXIT_BAD_ARGS;
    }

    if (summary[0] == '\0') {
        fprintf(stderr, "Error: summary is empty\n");
        return SCRIBE_EXIT_BAD_ARGS;
    }

    /* SECURITY: Reject newlines in summary (Markdown injection) */
    int nrc = check_no_newline(summary, "summary");
    if (nrc != 0) return nrc;

    /* Parse options */
    scribe_entry_t entry;
    memset(&entry, 0, sizeof(entry));

    int ret = snprintf(entry.summary, sizeof(entry.summary), "%s", summary);
    int trc = check_snprintf(ret, sizeof(entry.summary), "summary");
    if (trc != 0) return trc;

    for (int i = 3; i < argc; i++) {
        ASSERT_MSG(argv[i] != NULL, "main: argv[%d] is NULL", i);

        if (strncmp(argv[i], "--chat-ref=", 11) == 0) {
            const char *val = argv[i] + 11;
            nrc = check_no_newline(val, "chat-ref");
            if (nrc != 0) return nrc;
            ret = snprintf(entry.chat_ref, sizeof(entry.chat_ref), "%s", val);
            trc = check_snprintf(ret, sizeof(entry.chat_ref), "chat-ref");
            if (trc != 0) return trc;
        } else if (strncmp(argv[i], "--participants=", 15) == 0) {
            const char *val = argv[i] + 15;
            nrc = check_no_newline(val, "participants");
            if (nrc != 0) return nrc;
            ret = snprintf(entry.participants, sizeof(entry.participants),
                           "%s", val);
            trc = check_snprintf(ret, sizeof(entry.participants),
                                 "participants");
            if (trc != 0) return trc;
        } else if (strncmp(argv[i], "--artefacts=", 12) == 0) {
            const char *val = argv[i] + 12;
            nrc = check_no_newline(val, "artefacts");
            if (nrc != 0) return nrc;
            ret = snprintf(entry.artefacts, sizeof(entry.artefacts),
                           "%s", val);
            trc = check_snprintf(ret, sizeof(entry.artefacts), "artefacts");
            if (trc != 0) return trc;
        } else if (strncmp(argv[i], "--risk-tags=", 12) == 0) {
            const char *val = argv[i] + 12;
            nrc = check_no_newline(val, "risk-tags");
            if (nrc != 0) return nrc;
            ret = snprintf(entry.risk_tags, sizeof(entry.risk_tags),
                           "%s", val);
            trc = check_snprintf(ret, sizeof(entry.risk_tags), "risk-tags");
            if (trc != 0) return trc;
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
            ret = snprintf(entry.status, sizeof(entry.status), "%s", val);
            trc = check_snprintf(ret, sizeof(entry.status), "status");
            if (trc != 0) return trc;
        } else if (strncmp(argv[i], "--rationale=", 12) == 0) {
            const char *val = argv[i] + 12;
            nrc = check_no_newline(val, "rationale");
            if (nrc != 0) return nrc;
            ret = snprintf(entry.rationale, sizeof(entry.rationale),
                           "%s", val);
            trc = check_snprintf(ret, sizeof(entry.rationale), "rationale");
            if (trc != 0) return trc;
        } else if (strncmp(argv[i], "--supersedes=", 13) == 0) {
            const char *val = argv[i] + 13;
            nrc = check_no_newline(val, "supersedes");
            if (nrc != 0) return nrc;
            ret = snprintf(entry.supersedes, sizeof(entry.supersedes),
                           "%s", val);
            trc = check_snprintf(ret, sizeof(entry.supersedes), "supersedes");
            if (trc != 0) return trc;
        } else if (strncmp(argv[i], "--bus-dir=", 10) == 0) {
            const char *val = argv[i] + 10;
            /* SECURITY: Validate bus-dir for newline injection (S11) */
            nrc = check_no_newline(val, "bus-dir");
            if (nrc != 0) return nrc;
            ret = snprintf(entry.bus_dir, sizeof(entry.bus_dir),
                           "%s", val);
            trc = check_snprintf(ret, sizeof(entry.bus_dir), "bus-dir");
            if (trc != 0) return trc;
        } else {
            fprintf(stderr, "Error: unknown option: %s\n", argv[i]);
            return SCRIBE_EXIT_BAD_ARGS;
        }
    }

    /* Apply default status if not provided (BUG: documented default
     * must be enforced, not just claimed in help text) */
    if (entry.status[0] == '\0') {
        snprintf(entry.status, sizeof(entry.status), "decided");
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

    /* Postcondition: return value must be a documented exit code */
    int rc = scribe_log_append(log_path, &entry);
    ASSERT_MSG(rc == SCRIBE_EXIT_OK || rc == SCRIBE_EXIT_ERROR
               || rc == SCRIBE_EXIT_BAD_ARGS,
               "scribe_log_append returned undocumented exit code: %d", rc);
    return rc;
}
