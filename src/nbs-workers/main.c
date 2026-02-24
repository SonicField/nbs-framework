/*
 * main.c — nbs-workers entry point and command dispatch.
 *
 * Parses command-line arguments and dispatches to the appropriate
 * command handler in worker.c. Passes the current working directory
 * to all commands for path resolution.
 *
 * Usage: nbs-workers <command> [args...]
 *
 * Exit codes:
 *   0 = success
 *   1 = general error
 *   2 = not found
 *   4 = bad arguments
 */

#include "worker.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    /* Get current working directory — all path construction uses this */
    char cwd[PATH_BUF_SIZE];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "Error: getcwd() failed\n");
        return EXIT_ERROR;
    }

    if (argc < 2) {
        cmd_help();
        return EXIT_SUCCESS_CODE;
    }

    const char *command = argv[1];

    /* --- spawn --- */
    if (strcmp(command, "spawn") == 0) {
        if (argc < 5) {
            fprintf(stderr,
                    "Error: spawn requires <slug> <project-dir> "
                    "<task-description>\n"
                    "Usage: nbs-workers spawn <slug> <project-dir> "
                    "<task-description>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_spawn(argv[2], argv[3], argv[4], cwd);
    }

    /* --- status --- */
    if (strcmp(command, "status") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: status requires <name>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_status(argv[2], cwd);
    }

    /* --- search --- */
    if (strcmp(command, "search") == 0) {
        if (argc < 4) {
            fprintf(stderr,
                    "Error: search requires <name> <regex> [--context=N]\n");
            return EXIT_BAD_ARGS;
        }

        int context = 50; /* default */

        /* Parse optional --context=N */
        for (int i = 4; i < argc; i++) {
            if (strncmp(argv[i], "--context=", 10) == 0) {
                char *endptr;
                long parsed = strtol(argv[i] + 10, &endptr, 10);
                if (*endptr == '\0' && parsed >= 0 && parsed <= 10000)
                    context = (int)parsed;
            } else {
                fprintf(stderr,
                        "Warning: unknown argument ignored: %s\n", argv[i]);
            }
        }

        return cmd_search(argv[2], argv[3], context, cwd);
    }

    /* --- results --- */
    if (strcmp(command, "results") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: results requires <name>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_results(argv[2], cwd);
    }

    /* --- dismiss --- */
    if (strcmp(command, "dismiss") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: dismiss requires <name>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_dismiss(argv[2], cwd);
    }

    /* --- continue --- */
    if (strcmp(command, "continue") == 0) {
        if (argc < 3) {
            fprintf(stderr,
                    "Error: continue requires <handle>\n"
                    "Usage: nbs-workers continue <handle> [--model=MODEL]\n");
            return EXIT_BAD_ARGS;
        }

        const char *model_override = NULL;

        /* Parse optional --model=MODEL */
        for (int i = 3; i < argc; i++) {
            if (strncmp(argv[i], "--model=", 8) == 0) {
                model_override = argv[i] + 8;
            } else {
                fprintf(stderr,
                        "Error: unknown argument: %s\n", argv[i]);
                return EXIT_BAD_ARGS;
            }
        }

        return cmd_continue(argv[2], model_override, cwd);
    }

    /* --- session --- */
    if (strcmp(command, "session") == 0) {
        if (argc < 3) {
            fprintf(stderr,
                    "Error: session requires <handle>\n"
                    "Usage: nbs-workers session <handle>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_session(argv[2], cwd);
    }

    /* --- list --- */
    if (strcmp(command, "list") == 0) {
        return cmd_list(cwd);
    }

    /* --- help --- */
    if (strcmp(command, "help") == 0 ||
        strcmp(command, "--help") == 0 ||
        strcmp(command, "-h") == 0) {
        cmd_help();
        return EXIT_SUCCESS_CODE;
    }

    /* Unknown command */
    fprintf(stderr, "Unknown command: %s\n", command);
    fprintf(stderr, "Run 'nbs-workers help' for usage\n");
    return EXIT_BAD_ARGS;
}
