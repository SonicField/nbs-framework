/*
 * remote.c — nbs-chat-remote: SSH proxy for nbs-chat
 *
 * Drop-in replacement for nbs-chat that executes commands on a remote
 * machine via SSH. Same CLI, same exit codes, same stdout/stderr.
 *
 * Configuration (environment variables):
 *   NBS_CHAT_HOST  (required) — SSH target, e.g. "user@server"
 *   NBS_CHAT_PORT  (optional) — SSH port, default 22
 *   NBS_CHAT_KEY   (optional) — path to SSH identity file
 *   NBS_CHAT_BIN   (optional) — remote nbs-chat path, default "nbs-chat"
 *   NBS_CHAT_OPTS  (optional) — comma-separated SSH -o options
 *
 * Exit codes mirror nbs-chat exactly (0-4), with SSH failures mapped to 1.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Assertion macro (standalone, no chat_file.h dependency) ─────── */

#define ASSERT_MSG(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "ASSERTION FAILED: %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
            abort(); \
        } \
    } while (0)

/* ── Configuration ───────────────────────────────────────────────── */

typedef struct {
    const char *host;        /* NBS_CHAT_HOST (required) */
    int port;                /* NBS_CHAT_PORT (default 22) */
    const char *key_path;    /* NBS_CHAT_KEY (optional) */
    const char *remote_bin;  /* NBS_CHAT_BIN (default "nbs-chat") */
    const char *ssh_opts;    /* NBS_CHAT_OPTS (optional) — comma-separated -o options */
} remote_config_t;

/*
 * load_config — Load SSH configuration from environment variables.
 *
 * Preconditions:  cfg != NULL
 * Postconditions: On success (0), cfg->host is non-NULL and non-empty.
 *                 On failure (4), error printed to stderr.
 */
static int load_config(remote_config_t *cfg)
{
    ASSERT_MSG(cfg != NULL, "load_config: cfg is NULL");

    cfg->host = getenv("NBS_CHAT_HOST");
    if (!cfg->host || cfg->host[0] == '\0') {
        fprintf(stderr, "Error: NBS_CHAT_HOST environment variable not set\n");
        fprintf(stderr, "Set it to the SSH target, e.g.: export NBS_CHAT_HOST=user@server\n");
        return 4;
    }

    cfg->port = 22;
    const char *port_str = getenv("NBS_CHAT_PORT");
    if (port_str && port_str[0] != '\0') {
        char *endptr;
        errno = 0;
        long val = strtol(port_str, &endptr, 10);
        if (errno != 0 || *endptr != '\0' || val < 1 || val > 65535) {
            fprintf(stderr, "Error: Invalid NBS_CHAT_PORT: %s\n", port_str);
            return 4;
        }
        cfg->port = (int)val;
    }

    cfg->key_path = getenv("NBS_CHAT_KEY");
    if (cfg->key_path && cfg->key_path[0] == '\0')
        cfg->key_path = NULL;

    cfg->remote_bin = getenv("NBS_CHAT_BIN");
    if (!cfg->remote_bin || cfg->remote_bin[0] == '\0')
        cfg->remote_bin = "nbs-chat";

    cfg->ssh_opts = getenv("NBS_CHAT_OPTS");
    if (cfg->ssh_opts && cfg->ssh_opts[0] == '\0')
        cfg->ssh_opts = NULL;

    /* Postcondition: on success, host is non-NULL and non-empty */
    ASSERT_MSG(cfg->host != NULL && cfg->host[0] != '\0',
               "load_config postcondition: host is NULL or empty after successful load");
    ASSERT_MSG(cfg->port >= 1 && cfg->port <= 65535,
               "load_config postcondition: port %d out of range [1, 65535]", cfg->port);
    ASSERT_MSG(cfg->remote_bin != NULL && cfg->remote_bin[0] != '\0',
               "load_config postcondition: remote_bin is NULL or empty");

    return 0;
}

/* ── Shell escaping ──────────────────────────────────────────────── */

/*
 * shell_escape — Escape a string for safe passage through a remote shell.
 *
 * Wraps the argument in single quotes, escaping embedded single quotes
 * as '\'' (the standard POSIX idiom: end quote, escaped quote, start quote).
 *
 * Preconditions:  arg != NULL, buf != NULL, buf_size > 0
 * Postconditions: buf contains the escaped string, NUL-terminated.
 *                 Returns length of escaped string, or -1 if buffer too small.
 *
 * Example: "it's" → "'it'\''s'"
 */
static int shell_escape(const char *arg, char *buf, size_t buf_size)
{
    ASSERT_MSG(arg != NULL, "shell_escape: arg is NULL");
    ASSERT_MSG(buf != NULL, "shell_escape: buf is NULL");
    ASSERT_MSG(buf_size > 0, "shell_escape: buf_size is 0");

    size_t pos = 0;

    /* Opening quote */
    if (pos >= buf_size - 1) return -1;
    buf[pos++] = '\'';

    for (const char *p = arg; *p != '\0'; p++) {
        if (*p == '\'') {
            /* Need 4 chars: '\'' */
            if (pos + 4 >= buf_size - 1) return -1;
            buf[pos++] = '\'';   /* end current quote */
            buf[pos++] = '\\';
            buf[pos++] = '\'';   /* escaped literal quote */
            buf[pos++] = '\'';   /* restart quote */
        } else {
            if (pos >= buf_size - 1) return -1;
            buf[pos++] = *p;
        }
    }

    /* Closing quote */
    if (pos >= buf_size - 1) return -1;
    buf[pos++] = '\'';

    buf[pos] = '\0';

    /* Guard against size_t -> int truncation */
    if (pos > (size_t)INT_MAX) return -1;

    return (int)pos;
}

/* ── Command construction ────────────────────────────────────────── */

/*
 * contains_shell_metachar — Check if a string contains shell injection characters.
 *
 * Rejects strings containing: ; ` $ ( ) | & < > { } ! \ newline
 * These could be used for command injection when passed to SSH -o options.
 *
 * Returns 1 if dangerous characters found, 0 if clean.
 */
static int contains_shell_metachar(const char *s)
{
    ASSERT_MSG(s != NULL, "contains_shell_metachar: s is NULL");
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
        case ';': case '`': case '$': case '(': case ')':
        case '|': case '&': case '<': case '>': case '{':
        case '}': case '!': case '\\': case '\n': case '\r':
            return 1;
        default:
            break;
        }
    }
    return 0;
}

/*
 * build_ssh_argv — Construct the ssh command argument vector.
 *
 * SSH executes remote commands by concatenating args and passing them
 * to the remote shell. We construct a single properly-escaped remote
 * command string to prevent argument splitting.
 *
 * Preconditions:  cfg->host != NULL, chat_argc >= 2, chat_argv != NULL
 * Postconditions: Returns heap-allocated, NULL-terminated argv array.
 *                 argv[0] = "ssh", last element = NULL.
 *                 Caller must free argv, the remote command string (last non-NULL),
 *                 port_str_out (if non-NULL), and *opts_out (if non-NULL).
 *
 * Returns NULL on allocation failure or if inputs are invalid.
 */
static char **build_ssh_argv(const remote_config_t *cfg,
                              int chat_argc, char **chat_argv,
                              char **opts_out, char **port_str_out)
{
    ASSERT_MSG(cfg != NULL, "build_ssh_argv: cfg is NULL");
    ASSERT_MSG(cfg->host != NULL, "build_ssh_argv: cfg->host is NULL");
    ASSERT_MSG(chat_argc >= 2, "build_ssh_argv: chat_argc < 2, got %d", chat_argc);
    ASSERT_MSG(chat_argv != NULL, "build_ssh_argv: chat_argv is NULL");
    ASSERT_MSG(opts_out != NULL, "build_ssh_argv: opts_out is NULL");
    ASSERT_MSG(port_str_out != NULL, "build_ssh_argv: port_str_out is NULL");

    *opts_out = NULL;
    *port_str_out = NULL;

    /*
     * Build the remote command string with shell escaping.
     * Format: 'nbs-chat' 'arg1' 'arg2' ...
     *
     * Each arg can expand to at most 4x its length (every char is a single
     * quote needing '\'' = 4 chars) plus 2 for surrounding quotes plus 1 space.
     */
    size_t remote_cmd_size = 0;

    /* Size for the remote binary name — with overflow guard */
    size_t bin_len = strlen(cfg->remote_bin);
    if (bin_len > SIZE_MAX / 4 - 3) {
        fprintf(stderr, "Error: remote binary name too long\n");
        return NULL;
    }
    remote_cmd_size += bin_len * 4 + 3; /* worst case escape + quotes + space */

    /* Size for each nbs-chat argument (skip argv[0] which is our binary name) */
    for (int i = 1; i < chat_argc; i++) {
        size_t arg_len = strlen(chat_argv[i]);
        if (arg_len > SIZE_MAX / 4 - 3) {
            fprintf(stderr, "Error: argument %d too long\n", i);
            return NULL;
        }
        size_t arg_size = arg_len * 4 + 3;
        if (remote_cmd_size > SIZE_MAX - arg_size) {
            fprintf(stderr, "Error: total command size overflow\n");
            return NULL;
        }
        remote_cmd_size += arg_size;
    }
    if (remote_cmd_size > SIZE_MAX - 1) {
        fprintf(stderr, "Error: total command size overflow\n");
        return NULL;
    }
    remote_cmd_size += 1; /* NUL */

    char *remote_cmd = malloc(remote_cmd_size);
    if (!remote_cmd) return NULL;

    size_t cmd_pos = 0;

    /* Dynamically allocate escape buffer sized to the largest argument */
    size_t max_esc_needed = bin_len * 4 + 3;
    for (int i = 1; i < chat_argc; i++) {
        size_t need = strlen(chat_argv[i]) * 4 + 3;
        if (need > max_esc_needed) max_esc_needed = need;
    }
    char *esc_buf = malloc(max_esc_needed);
    if (!esc_buf) { free(remote_cmd); return NULL; }

    /* Escape and append remote binary name */
    int elen = shell_escape(cfg->remote_bin, esc_buf, max_esc_needed);
    if (elen < 0) {
        fprintf(stderr, "Error: failed to escape remote binary name '%s'\n",
                cfg->remote_bin);
        free(esc_buf);
        free(remote_cmd);
        return NULL;
    }
    memcpy(remote_cmd + cmd_pos, esc_buf, (size_t)elen);
    cmd_pos += (size_t)elen;

    /* Escape and append each argument */
    for (int i = 1; i < chat_argc; i++) {
        remote_cmd[cmd_pos++] = ' ';
        elen = shell_escape(chat_argv[i], esc_buf, max_esc_needed);
        if (elen < 0) {
            fprintf(stderr, "Error: failed to escape argument %d '%s'\n",
                    i, chat_argv[i]);
            free(esc_buf);
            free(remote_cmd);
            return NULL;
        }
        memcpy(remote_cmd + cmd_pos, esc_buf, (size_t)elen);
        cmd_pos += (size_t)elen;
    }
    remote_cmd[cmd_pos] = '\0';

    free(esc_buf);

    /*
     * Build SSH argv.
     * Maximum elements: ssh -p PORT -i KEY -o OPT1 -o OPT2 -o OPT3 -o OPT4 host REMOTE_CMD NULL
     *                    1    2       2      2       2       2       2       1     1          1 = 16
     */
    const int max_args = 16;
    char **argv = calloc((size_t)max_args, sizeof(char *));
    if (!argv) { free(remote_cmd); return NULL; }

    int ai = 0;
    /* ai < max_args checked before every write */
    ASSERT_MSG(ai < max_args, "build_ssh_argv: argv overflow before 'ssh'");
    argv[ai++] = (char *)"ssh";

    /* Port (only if non-default) — heap-allocated to avoid dangling stack pointer */
    if (cfg->port != 22) {
        char port_buf[8];
        snprintf(port_buf, sizeof(port_buf), "%d", cfg->port);
        char *port_heap = strdup(port_buf);
        if (!port_heap) { free(remote_cmd); free(argv); return NULL; }
        *port_str_out = port_heap;

        ASSERT_MSG(ai + 1 < max_args,
                   "build_ssh_argv: argv overflow before '-p PORT', ai=%d", ai);
        argv[ai++] = (char *)"-p";
        argv[ai++] = port_heap;
    }

    /* Identity file */
    if (cfg->key_path) {
        ASSERT_MSG(ai + 1 < max_args,
                   "build_ssh_argv: argv overflow before '-i KEY', ai=%d", ai);
        argv[ai++] = (char *)"-i";
        argv[ai++] = (char *)cfg->key_path;
    }

    /* Extra SSH options (comma-separated, each becomes -o <option>) */
    if (cfg->ssh_opts) {
        /*
         * SECURITY: Validate SSH options to prevent command injection.
         * NBS_CHAT_OPTS is an environment variable under the user's control,
         * but defence-in-depth: reject options containing shell metacharacters
         * that could be exploited via ProxyCommand or similar directives.
         */
        if (contains_shell_metachar(cfg->ssh_opts)) {
            fprintf(stderr, "Error: NBS_CHAT_OPTS contains dangerous characters "
                    "(;`$()| etc.) — refusing to proceed\n");
            free(remote_cmd);
            free(*port_str_out);
            *port_str_out = NULL;
            free(argv);
            return NULL;
        }

        /* Make a mutable copy for strtok — caller frees via opts_out */
        char *opts_buf = strdup(cfg->ssh_opts);
        if (opts_buf) {
            *opts_out = opts_buf;
            char *saveptr = NULL;
            char *opt = strtok_r(opts_buf, ",", &saveptr);
            int opt_count = 0;
            while (opt && opt_count < 4) {
                /* Skip leading whitespace */
                while (*opt == ' ') opt++;
                if (*opt != '\0') {
                    ASSERT_MSG(ai + 1 < max_args,
                               "build_ssh_argv: argv overflow before '-o OPT', ai=%d", ai);
                    argv[ai++] = (char *)"-o";
                    argv[ai++] = opt;
                    opt_count++;
                }
                opt = strtok_r(NULL, ",", &saveptr);
            }
        }
    }

    /* Host */
    ASSERT_MSG(ai < max_args,
               "build_ssh_argv: argv overflow before host, ai=%d", ai);
    argv[ai++] = (char *)cfg->host;

    /* Remote command (single string, properly escaped) */
    ASSERT_MSG(ai < max_args,
               "build_ssh_argv: argv overflow before remote_cmd, ai=%d", ai);
    argv[ai++] = remote_cmd;

    ASSERT_MSG(ai < max_args,
               "build_ssh_argv: no room for NULL terminator, ai=%d", ai);
    argv[ai] = NULL;

    return argv;
}

/* ── SSH execution ───────────────────────────────────────────────── */

/*
 * run_ssh — Execute the SSH command, proxy stdout/stderr, return exit code.
 *
 * Uses fork+execvp (same pattern as hub_commands.c run_passthrough).
 * stdout and stderr pass through directly — no capture.
 *
 * Preconditions:  argv != NULL, argv[0] = "ssh", host != NULL
 * Postconditions: Returns remote exit code (0-254), or 1 on SSH/exec failure.
 */
static int run_ssh(char **argv, const char *host)
{
    ASSERT_MSG(argv != NULL, "run_ssh: argv is NULL");
    ASSERT_MSG(argv[0] != NULL, "run_ssh: argv[0] is NULL");
    ASSERT_MSG(host != NULL, "run_ssh: host is NULL");

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: fork failed: %s\n", strerror(errno));
        return 1;
    }

    if (pid == 0) {
        /* Child: exec ssh */
        execvp("ssh", argv);
        fprintf(stderr, "Error: Failed to execute ssh: %s\n", strerror(errno));
        _exit(127);
    }

    /* Parent: wait for child, retrying on EINTR (e.g. from SIGCHLD) */
    int status;
    pid_t wpid;
    do {
        wpid = waitpid(pid, &status, 0);
    } while (wpid < 0 && errno == EINTR);

    if (wpid < 0) {
        fprintf(stderr, "Error: waitpid failed: %s\n", strerror(errno));
        return 1;
    }

    if (!WIFEXITED(status)) {
        fprintf(stderr, "Error: SSH process terminated abnormally\n");
        return 1;
    }

    int exit_code = WEXITSTATUS(status);

    /* SSH uses exit code 255 for its own errors (connection refused, auth failure, etc.) */
    if (exit_code == 255) {
        /* host is passed via %s to prevent format string injection */
        fprintf(stderr, "Error: SSH connection to %s failed\n", host);
        return 1;
    }

    /* Exit code 127 means execvp failed in child (ssh not found) */
    if (exit_code == 127) {
        fprintf(stderr, "Error: ssh command not found on PATH\n");
        return 1;
    }

    return exit_code;
}

/* ── Usage ───────────────────────────────────────────────────────── */

static void print_usage(void)
{
    printf("nbs-chat-remote: SSH proxy for nbs-chat\n\n");
    printf("Usage: nbs-chat-remote <command> [args...]\n\n");
    printf("Commands (identical to nbs-chat):\n");
    printf("  create <file>                    Create new chat file\n");
    printf("  send <file> <handle> <message>   Send a message\n");
    printf("  read <file> [options]            Read messages\n");
    printf("  poll <file> <handle> [options]   Wait for new message\n");
    printf("  participants <file>              List participants and counts\n");
    printf("  help                             Show this help\n\n");
    printf("Environment variables:\n");
    printf("  NBS_CHAT_HOST  (required) SSH target, e.g. user@server\n");
    printf("  NBS_CHAT_PORT  (optional) SSH port (default: 22)\n");
    printf("  NBS_CHAT_KEY   (optional) Path to SSH identity file\n");
    printf("  NBS_CHAT_BIN   (optional) Remote nbs-chat path (default: nbs-chat)\n");
    printf("  NBS_CHAT_OPTS  (optional) Comma-separated SSH -o options\n\n");
    printf("All commands are executed on the remote machine via SSH.\n");
    printf("File paths refer to paths on the remote machine.\n\n");
    printf("Exit codes:\n");
    printf("  0 - Success\n");
    printf("  1 - General error (including SSH failures)\n");
    printf("  2 - File not found / already exists\n");
    printf("  3 - Timeout (poll only)\n");
    printf("  4 - Invalid arguments / missing configuration\n");
}

/* ── Entry point ─────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    ASSERT_MSG(argc >= 1, "main: argc must be at least 1, got %d", argc);
    ASSERT_MSG(argv != NULL, "main: argv is NULL");

    if (argc < 2) {
        fprintf(stderr, "Error: No command specified\n");
        fprintf(stderr, "Run 'nbs-chat-remote help' for usage\n");
        return 4;
    }

    /* Handle help locally — no SSH needed */
    if (strcmp(argv[1], "help") == 0) {
        print_usage();
        return 0;
    }

    /* Load SSH configuration from environment */
    remote_config_t cfg;
    int rc = load_config(&cfg);
    if (rc != 0) return rc;

    /* Build SSH command with shell-escaped arguments */
    char *opts_buf = NULL;
    char *port_str_heap = NULL;
    char **ssh_argv = build_ssh_argv(&cfg, argc, argv, &opts_buf, &port_str_heap);
    if (!ssh_argv) {
        fprintf(stderr, "Error: Failed to allocate SSH command\n");
        return 1;
    }

    /* Execute and proxy exit code */
    int exit_code = run_ssh(ssh_argv, cfg.host);

    /*
     * Cleanup: free heap-allocated resources.
     * The remote_cmd is the last non-NULL element in argv.
     * port_str_heap and opts_buf are tracked explicitly (not fragile search).
     */
    for (int i = 0; ssh_argv[i] != NULL; i++) {
        if (ssh_argv[i + 1] == NULL) {
            free(ssh_argv[i]); /* This is the malloc'd remote_cmd */
            break;
        }
    }
    free(port_str_heap);
    free(opts_buf);
    free(ssh_argv);

    return exit_code;
}
