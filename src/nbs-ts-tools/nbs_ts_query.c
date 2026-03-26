/*
 * nbs_ts_query.c — Extract line ranges from agent session logs.
 *
 * Resolves agent name + chat tag → nbs-ts session handle, reads the
 * session's output.log, strips ANSI escapes, and prints lines in the
 * requested range as clean text.
 *
 * Usage:
 *   nbs-ts-query <chat-tag> <agent> --from=N --to=N
 *
 * Exit codes:
 *   0 - Success
 *   1 - Session not found
 *   4 - Invalid arguments
 */

#define _GNU_SOURCE

#include "strip_ansi.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PATH 8192
#define MAX_LINE 4096

/* --- Session resolution (same as nbs_ts_grep.c) --- */

static int find_session(const char *chat_tag, const char *agent,
                         char *output_log, size_t log_size)
{
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') return -1;

    char sessions_dir[MAX_PATH];
    int n = snprintf(sessions_dir, sizeof(sessions_dir),
                     "%s/.nbs-ts/sessions", home);
    if (n < 0 || (size_t)n >= sizeof(sessions_dir)) return -1;

    /* Build match prefix: nbs-<agent>- */
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "nbs-%s-", agent);

    DIR *dir = opendir(sessions_dir);
    if (!dir) return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char sess_dir[MAX_PATH];
        snprintf(sess_dir, sizeof(sess_dir), "%s/%s",
                 sessions_dir, ent->d_name);

        /* Read name */
        char name_path[MAX_PATH];
        snprintf(name_path, sizeof(name_path), "%s/name", sess_dir);
        FILE *nf = fopen(name_path, "r");
        if (!nf) continue;
        char name[256] = "";
        if (!fgets(name, sizeof(name), nf)) { fclose(nf); continue; }
        fclose(nf);
        char *nl = strchr(name, '\n');
        if (nl) *nl = '\0';

        /* Check name matches: starts with prefix and contains tag */
        if (strncmp(name, prefix, strlen(prefix)) != 0) continue;
        if (strstr(name, chat_tag) == NULL) continue;

        /* Check alive */
        char pid_path[MAX_PATH];
        snprintf(pid_path, sizeof(pid_path), "%s/pid", sess_dir);
        FILE *pf = fopen(pid_path, "r");
        if (!pf) continue;
        char pid_str[32] = "";
        if (!fgets(pid_str, sizeof(pid_str), pf)) { fclose(pf); continue; }
        fclose(pf);
        long pid = strtol(pid_str, NULL, 10);
        if (pid <= 0) continue;
        if (kill((pid_t)pid, 0) != 0) continue;

        /* Found it */
        snprintf(output_log, log_size, "%s/output.log", sess_dir);
        closedir(dir);
        return 0;
    }
    closedir(dir);
    return -1;
}

/* --- Main --- */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage: nbs-ts-query <chat-tag> <agent>"
            " --from=N --to=N\n");
        return 4;
    }

    const char *chat_tag = argv[1];
    const char *agent = argv[2];
    int from_line = 0;
    int to_line = 0;

    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "--from=", 7) == 0)
            from_line = atoi(argv[i] + 7);
        else if (strncmp(argv[i], "--to=", 5) == 0)
            to_line = atoi(argv[i] + 5);
        else {
            fprintf(stderr, "nbs-ts-query: unknown option: %s\n", argv[i]);
            return 4;
        }
    }

    if (from_line <= 0 || to_line <= 0 || to_line < from_line) {
        fprintf(stderr, "nbs-ts-query: --from and --to required,"
                " --to must be >= --from\n");
        return 4;
    }

    char output_log[MAX_PATH];
    if (find_session(chat_tag, agent, output_log, sizeof(output_log)) != 0) {
        fprintf(stderr, "nbs-ts-query: no alive session for"
                " agent '%s' tag '%s'\n", agent, chat_tag);
        return 1;
    }

    FILE *f = fopen(output_log, "r");
    if (!f) {
        fprintf(stderr, "nbs-ts-query: cannot open %s: %s\n",
                output_log, strerror(errno));
        return 1;
    }

    int line_num = 0;
    char buf[MAX_LINE];
    while (fgets(buf, sizeof(buf), f)) {
        line_num++;
        if (line_num < from_line) continue;
        if (line_num > to_line) break;

        strip_ansi(buf);

        /* Print non-blank lines with line numbers */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        if (buf[0] != '\0')
            printf("%d:%s\n", line_num, buf);
    }
    fclose(f);
    return 0;
}
