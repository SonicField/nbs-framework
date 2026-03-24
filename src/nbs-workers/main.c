/*
 * main.c -- nbs-workers entry point and command dispatch.
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
#include <errno.h>

int main(int argc, char *argv[])
{
    ASSERT_MSG(argv != NULL, "main: argv is NULL");
    ASSERT_MSG(argc >= 1, "main: argc must be >= 1, got %d", argc);

    /* Precondition: all argv entries are non-NULL */
    for (int i = 0; i < argc; i++) {
        ASSERT_MSG(argv[i] != NULL, "main: argv[%d] is NULL", i);
    }

    /* Get current working directory -- all path construction uses this */
    char cwd[PATH_BUF_SIZE];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "Error: getcwd() failed (errno=%d: %s). "
                "Cannot determine working directory for path resolution.\n",
                errno, strerror(errno));
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
                    "[--skill=FILE] <task-description>\n");
            return EXIT_BAD_ARGS;
        }

        /* Parse optional --skill=FILE before the task description */
        const char *skill_file = NULL;
        int task_arg = 4;
        for (int i = 4; i < argc; i++) {
            if (strncmp(argv[i], "--skill=", 8) == 0) {
                skill_file = argv[i] + 8;
                task_arg = i + 1;
            }
        }
        if (task_arg >= argc) {
            fprintf(stderr, "Error: missing task-description after --skill\n");
            return EXIT_BAD_ARGS;
        }

        /* If --skill given, read file and prepend to task description */
        char combined_task[65536];
        const char *task_desc = argv[task_arg];
        if (skill_file) {
            FILE *sf = fopen(skill_file, "r");
            if (!sf) {
                /* Try ~/.nbs/<skill_file> */
                char alt_path[PATH_BUF_SIZE];
                const char *home = getenv("HOME");
                if (home) {
                    snprintf(alt_path, sizeof(alt_path), "%s/.nbs/%s", home, skill_file);
                    sf = fopen(alt_path, "r");
                }
                if (!sf) {
                    fprintf(stderr, "Error: skill file not found: %s\n", skill_file);
                    return EXIT_BAD_ARGS;
                }
            }
            size_t skill_len = fread(combined_task, 1, sizeof(combined_task) - 2, sf);
            fclose(sf);
            combined_task[skill_len] = '\n';
            size_t remaining = sizeof(combined_task) - skill_len - 1;
            size_t task_len = strlen(argv[task_arg]);
            if (task_len >= remaining) task_len = remaining - 1;
            memcpy(combined_task + skill_len + 1, argv[task_arg], task_len);
            combined_task[skill_len + 1 + task_len] = '\0';
            task_desc = combined_task;
        }

        return cmd_spawn(argv[2], argv[3], task_desc, cwd);
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
                errno = 0;
                char *endptr;
                long parsed = strtol(argv[i] + 10, &endptr, 10);
                if (*endptr == '\0' && errno != ERANGE &&
                    parsed >= 0 && parsed <= 10000) {
                    context = (int)parsed;
                } else {
                    fprintf(stderr,
                            "Error: invalid --context value: %s "
                            "(expected integer 0-10000)\n", argv[i] + 10);
                    return EXIT_BAD_ARGS;
                }
            } else {
                fprintf(stderr,
                        "Error: unknown argument: %s\n", argv[i]);
                return EXIT_BAD_ARGS;
            }
        }

        /* Postcondition: context is in valid range */
        ASSERT_MSG(context >= 0 && context <= 10000,
                   "search: context out of range after parsing: %d", context);

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
