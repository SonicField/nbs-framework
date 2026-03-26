/*
 * nbs_ts_grep.c — Search agent session logs for patterns.
 *
 * Resolves agent name + chat tag → nbs-ts session handle, reads the
 * session's output.log, strips ANSI escapes, and greps for a pattern.
 *
 * Usage:
 *   nbs-ts-grep <pattern> <chat-tag> <agent|--all> [--from=N] [--to=N]
 *
 * Exit codes:
 *   0 - Matches found
 *   1 - No matches
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
#define MAX_SESSIONS 64

/* --- Session resolution --- */

typedef struct {
    char handle[64];
    char name[256];
    char output_log[MAX_PATH];
} session_t;

/*
 * find_sessions — Find alive nbs-ts sessions matching a name pattern.
 *
 * Scans ~/.nbs-ts/sessions/ directories. For each, reads the "name"
 * file and checks if the session is alive (pid file exists, process
 * running). Returns count of matching sessions.
 */
static int find_sessions(const char *name_pattern, session_t *out,
                          int max_out)
{
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') return 0;

    char sessions_dir[MAX_PATH];
    int n = snprintf(sessions_dir, sizeof(sessions_dir),
                     "%s/.nbs-ts/sessions", home);
    if (n < 0 || (size_t)n >= sizeof(sessions_dir)) return 0;

    DIR *dir = opendir(sessions_dir);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_out) {
        if (ent->d_name[0] == '.') continue;

        char sess_dir[MAX_PATH];
        snprintf(sess_dir, sizeof(sess_dir), "%s/%s",
                 sessions_dir, ent->d_name);

        /* Read name file */
        char name_path[MAX_PATH];
        snprintf(name_path, sizeof(name_path), "%s/name", sess_dir);
        FILE *nf = fopen(name_path, "r");
        if (!nf) continue;
        char name[256] = "";
        if (!fgets(name, sizeof(name), nf)) { fclose(nf); continue; }
        fclose(nf);
        /* Strip newline */
        char *nl = strchr(name, '\n');
        if (nl) *nl = '\0';

        /* Check name matches pattern (substring) */
        if (name_pattern[0] != '\0' && strstr(name, name_pattern) == NULL)
            continue;

        /* Check alive: pid file exists and process is running */
        char pid_path[MAX_PATH];
        snprintf(pid_path, sizeof(pid_path), "%s/pid", sess_dir);
        FILE *pf = fopen(pid_path, "r");
        if (!pf) continue;
        char pid_str[32] = "";
        if (!fgets(pid_str, sizeof(pid_str), pf)) { fclose(pf); continue; }
        fclose(pf);
        long pid = strtol(pid_str, NULL, 10);
        if (pid <= 0) continue;
        if (kill((pid_t)pid, 0) != 0) continue;  /* not alive */

        /* Found a live matching session */
        snprintf(out[count].handle, sizeof(out[count].handle),
                 "%s", ent->d_name);
        snprintf(out[count].name, sizeof(out[count].name), "%s", name);
        snprintf(out[count].output_log, sizeof(out[count].output_log),
                 "%s/output.log", sess_dir);
        count++;
    }
    closedir(dir);
    return count;
}

/* --- Grep a single session log --- */

static int grep_session(const session_t *sess, const char *pattern,
                         int from_line, int to_line, int print_name)
{
    FILE *f = fopen(sess->output_log, "r");
    if (!f) {
        fprintf(stderr, "nbs-ts-grep: cannot open %s: %s\n",
                sess->output_log, strerror(errno));
        return 0;
    }

    int matches = 0;
    int line_num = 0;
    char buf[MAX_LINE];

    while (fgets(buf, sizeof(buf), f)) {
        line_num++;
        if (from_line > 0 && line_num < from_line) continue;
        if (to_line > 0 && line_num > to_line) break;

        strip_ansi(buf);

        /* Skip blank lines after stripping */
        size_t len = strlen(buf);
        if (len == 0) continue;
        if (buf[len - 1] == '\n') buf[len - 1] = '\0';
        if (buf[0] == '\0') continue;

        /* Substring search */
        if (strstr(buf, pattern) != NULL) {
            if (print_name)
                printf("%s:%d:%s\n", sess->name, line_num, buf);
            else
                printf("%d:%s\n", line_num, buf);
            matches++;
        }
    }
    fclose(f);
    return matches;
}

/* --- Main --- */

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
            "Usage: nbs-ts-grep <pattern> <chat-tag> <agent|--all>"
            " [--from=N] [--to=N]\n");
        return 4;
    }

    const char *pattern = argv[1];
    const char *chat_tag = argv[2];
    const char *agent = argv[3];
    int from_line = 0;
    int to_line = 0;

    for (int i = 4; i < argc; i++) {
        if (strncmp(argv[i], "--from=", 7) == 0)
            from_line = atoi(argv[i] + 7);
        else if (strncmp(argv[i], "--to=", 5) == 0)
            to_line = atoi(argv[i] + 5);
        else {
            fprintf(stderr, "nbs-ts-grep: unknown option: %s\n", argv[i]);
            return 4;
        }
    }

    int search_all = (strcmp(agent, "--all") == 0);

    /* Build name pattern for session lookup */
    char name_pattern[512];
    if (search_all) {
        /* Match any session with this chat tag */
        snprintf(name_pattern, sizeof(name_pattern), "%s", chat_tag);
    } else {
        /* Match specific agent: nbs-<agent>-*-<tag> */
        snprintf(name_pattern, sizeof(name_pattern),
                 "nbs-%s-", agent);
        /* Also need to verify the tag matches — do it after finding */
    }

    session_t sessions[MAX_SESSIONS];
    int nsessions = find_sessions(name_pattern, sessions, MAX_SESSIONS);

    if (nsessions == 0) {
        fprintf(stderr, "nbs-ts-grep: no alive sessions matching '%s'\n",
                name_pattern);
        return 1;
    }

    /* If searching for a specific agent, filter by chat tag too */
    int total_matches = 0;
    for (int i = 0; i < nsessions; i++) {
        if (!search_all) {
            /* Verify the session name contains the chat tag */
            if (strstr(sessions[i].name, chat_tag) == NULL)
                continue;
        }
        total_matches += grep_session(&sessions[i], pattern,
                                       from_line, to_line,
                                       search_all || nsessions > 1);
    }

    return total_matches > 0 ? 0 : 1;
}
