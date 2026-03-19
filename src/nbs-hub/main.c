/*
 * main.c — nbs-hub entry point and command dispatch.
 *
 * Parses command-line arguments and dispatches to hub.c commands.
 * Supports --project <path> to override hub discovery.
 *
 * Usage: nbs-hub [--project <path>] <command> [args...]
 *
 * Exit codes: see hub.h
 */

#include "hub.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/*
 * cmd_phase_name — Set the current phase name.
 *
 * Not in hub.h because it's a trivial state_write.
 */
static int cmd_phase_name(const char *name, const char *project_dir)
{
    ASSERT_MSG(name != NULL, "cmd_phase_name: name is NULL");
    ASSERT_MSG(project_dir != NULL, "cmd_phase_name: project_dir is NULL");

    char state_path[PATH_BUF_SIZE];
    snprintf(state_path, sizeof(state_path), "%s/%s",
             project_dir, HUB_STATE);
    state_write(state_path, "phase_name", name);

    char log_msg[VALUE_BUF_SIZE];
    char phase_num[32] = "?";
    state_read(state_path, "phase_num", phase_num, sizeof(phase_num));
    snprintf(log_msg, sizeof(log_msg), "PHASE NAME: %s -- %s",
             phase_num, name);
    hub_log(project_dir, log_msg);

    printf("Phase %s: %s\n", phase_num, name);
    return EXIT_SUCCESS_CODE;
}

int main(int argc, char *argv[])
{
    ASSERT_MSG(argv != NULL, "main: argv is NULL");
    ASSERT_MSG(argc >= 1, "main: argc must be >= 1, got %d", argc);

    if (argc < 2) {
        cmd_help();
        return EXIT_SUCCESS_CODE;
    }

    /* Parse --project option */
    const char *override_dir = NULL;
    int arg_start = 1;

    if (strcmp(argv[1], "--project") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: --project requires a path\n");
            return EXIT_BAD_ARGS;
        }
        override_dir = argv[2];
        arg_start = 3;
    }

    if (arg_start >= argc) {
        cmd_help();
        return EXIT_SUCCESS_CODE;
    }

    const char *command = argv[arg_start];

    /* --- init does not require existing hub --- */
    if (strcmp(command, "init") == 0) {
        if (arg_start + 2 >= argc) {
            fprintf(stderr,
                    "Error: init requires <project-dir> <goal>\n"
                    "Usage: nbs-hub init <project-dir> <goal>\n");
            return EXIT_BAD_ARGS;
        }

        /* Resolve project dir to absolute path */
        char abs_dir[PATH_BUF_SIZE];
        if (argv[arg_start + 1][0] == '/') {
            snprintf(abs_dir, sizeof(abs_dir), "%s", argv[arg_start + 1]);
        } else {
            char cwd[PATH_BUF_SIZE];
            if (getcwd(cwd, sizeof(cwd)) == NULL) {
                fprintf(stderr, "Error: getcwd() failed\n");
                return EXIT_ERROR;
            }
            snprintf(abs_dir, sizeof(abs_dir), "%s/%s",
                     cwd, argv[arg_start + 1]);
        }

        return cmd_init(abs_dir, argv[arg_start + 2]);
    }

    /* --- help does not require hub --- */
    if (strcmp(command, "help") == 0 ||
        strcmp(command, "--help") == 0 ||
        strcmp(command, "-h") == 0) {
        cmd_help();
        return EXIT_SUCCESS_CODE;
    }

    /* --- All other commands require finding the hub --- */
    char project_dir[PATH_BUF_SIZE];

    if (override_dir) {
        snprintf(project_dir, sizeof(project_dir), "%s", override_dir);
        /* Verify hub exists there */
        char check[PATH_BUF_SIZE];
        snprintf(check, sizeof(check), "%s/%s", project_dir, HUB_SUBDIR);
        struct stat st;
        if (stat(check, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr,
                    "Error: no hub found at %s\n"
                    "Run: nbs-hub init <project-dir> <goal>\n",
                    check);
            return EXIT_NOT_FOUND;
        }
    } else {
        char cwd[PATH_BUF_SIZE];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            fprintf(stderr, "Error: getcwd() failed\n");
            return EXIT_ERROR;
        }
        if (find_project_dir(cwd, project_dir) != 0) {
            fprintf(stderr,
                    "Error: no hub found. Searched upward from %s\n"
                    "Run: nbs-hub init <project-dir> <goal>\n", cwd);
            return EXIT_NOT_FOUND;
        }
    }

    /* --- Dispatch --- */

    if (strcmp(command, "status") == 0) {
        return cmd_status(project_dir);
    }

    if (strcmp(command, "spawn") == 0) {
        if (arg_start + 2 >= argc) {
            fprintf(stderr,
                    "Error: spawn requires <slug> <task-description>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_spawn(argv[arg_start + 1], argv[arg_start + 2],
                         project_dir);
    }

    if (strcmp(command, "check") == 0) {
        if (arg_start + 1 >= argc) {
            fprintf(stderr, "Error: check requires <worker-name>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_check(argv[arg_start + 1], project_dir);
    }

    if (strcmp(command, "result") == 0) {
        if (arg_start + 1 >= argc) {
            fprintf(stderr, "Error: result requires <worker-name>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_result(argv[arg_start + 1], project_dir);
    }

    if (strcmp(command, "dismiss") == 0) {
        if (arg_start + 1 >= argc) {
            fprintf(stderr, "Error: dismiss requires <worker-name>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_dismiss(argv[arg_start + 1], project_dir);
    }

    if (strcmp(command, "list") == 0) {
        return cmd_list(project_dir);
    }

    if (strcmp(command, "audit") == 0) {
        if (arg_start + 1 >= argc) {
            fprintf(stderr, "Error: audit requires <file>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_audit(argv[arg_start + 1], project_dir);
    }

    if (strcmp(command, "gate") == 0) {
        if (arg_start + 3 >= argc) {
            fprintf(stderr,
                    "Error: gate requires <phase-name> "
                    "<test-results-file> <audit-file>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_gate(argv[arg_start + 1], argv[arg_start + 2],
                        argv[arg_start + 3], project_dir);
    }

    if (strcmp(command, "phase") == 0) {
        return cmd_phase(project_dir);
    }

    if (strcmp(command, "phase-name") == 0) {
        if (arg_start + 1 >= argc) {
            fprintf(stderr, "Error: phase-name requires <name>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_phase_name(argv[arg_start + 1], project_dir);
    }

    if (strcmp(command, "doc") == 0) {
        if (arg_start + 1 >= argc) {
            fprintf(stderr,
                    "Error: doc requires a subcommand "
                    "(register, list, read)\n");
            return EXIT_BAD_ARGS;
        }
        const char *sub = argv[arg_start + 1];

        if (strcmp(sub, "register") == 0) {
            if (arg_start + 3 >= argc) {
                fprintf(stderr,
                        "Error: doc register requires <name> <path>\n");
                return EXIT_BAD_ARGS;
            }
            return cmd_doc_register(argv[arg_start + 2],
                                    argv[arg_start + 3], project_dir);
        }
        if (strcmp(sub, "list") == 0) {
            return cmd_doc_list(project_dir);
        }
        if (strcmp(sub, "read") == 0) {
            if (arg_start + 2 >= argc) {
                fprintf(stderr, "Error: doc read requires <name>\n");
                return EXIT_BAD_ARGS;
            }
            return cmd_doc_read(argv[arg_start + 2], project_dir);
        }

        fprintf(stderr, "Unknown doc subcommand: %s\n", sub);
        return EXIT_BAD_ARGS;
    }

    if (strcmp(command, "decision") == 0) {
        if (arg_start + 1 >= argc) {
            fprintf(stderr, "Error: decision requires <text>\n");
            return EXIT_BAD_ARGS;
        }
        return cmd_decision(argv[arg_start + 1], project_dir);
    }

    if (strcmp(command, "log") == 0) {
        int n = MAX_LOG_DEFAULT;
        if (arg_start + 1 < argc) {
            n = atoi(argv[arg_start + 1]);
            if (n <= 0) n = MAX_LOG_DEFAULT;
        }
        return cmd_log(n, project_dir);
    }

    /* Unknown command */
    fprintf(stderr, "Unknown command: %s\n", command);
    fprintf(stderr, "Run 'nbs-hub help' for usage\n");
    return EXIT_BAD_ARGS;
}
