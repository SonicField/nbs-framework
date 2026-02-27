/*
 * main.c — nbs-sidecar entry point.
 *
 * Parses command-line arguments and environment variables into a
 * sidecar_config_t, initialises the transport, and calls sidecar_run().
 *
 * Usage:
 *   nbs-sidecar --handle=NAME --root=PATH --transport=tmux --pane-id=ID
 *   nbs-sidecar --handle=NAME --root=PATH --transport=pty --pty-path=PATH --session=NAME
 *
 * All parameters can also be set via NBS_* environment variables.
 * Command-line arguments take precedence over environment variables.
 */

#include "sidecar.h"
#include "transport.h"
#include "../nbs-common/nbs_assert.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* No timing interval should exceed ~27 hours (100000 seconds).
 * This bounds env_int values to a sane range for all config fields. */
#define ENV_INT_MAX 100000

static void print_usage(void) {
    fprintf(stderr,
        "nbs-sidecar: Claude Code session monitor\n\n"
        "Usage:\n"
        "  nbs-sidecar --handle=NAME --root=PATH --transport=tmux --pane-id=ID\n"
        "  nbs-sidecar --handle=NAME --root=PATH --transport=pty "
        "--pty-path=PATH --session=NAME\n\n"
        "Options:\n"
        "  --handle=NAME          Agent handle (required)\n"
        "  --root=PATH            Project root containing .nbs/ (required)\n"
        "  --transport=tmux|pty   Transport mode (required)\n"
        "  --pane-id=ID           tmux pane ID (tmux mode)\n"
        "  --pty-path=PATH        Path to pty-session binary (pty mode)\n"
        "  --session=NAME         pty-session session name (pty mode)\n"
        "  --initial-prompt=TEXT  Custom initial prompt\n"
        "  --log=PATH             Log file for sidecar stderr\n\n"
        "Environment (defaults, overridden by args):\n"
        "  NBS_HANDLE, NBS_ROOT, NBS_BUS_CHECK_INTERVAL (3),\n"
        "  NBS_NOTIFY_COOLDOWN (15), NBS_STARTUP_GRACE (30),\n"
        "  NBS_NOTIFY_FAIL_THRESHOLD (5), NBS_STANDUP_INTERVAL (15),\n"
        "  NBS_ACTIVE_HEARTBEAT (0), NBS_FLUSH_INTERVAL (60),\n"
        "  NBS_INITIAL_PROMPT, NBS_REMOTE_HOST, NBS_REMOTE_SSH_OPTS\n");
}

/*
 * env_int — Read an integer from an environment variable.
 *
 * Returns the value if set and valid, or default_val otherwise.
 */
static int env_int(const char *name, int default_val) {
    ASSERT_MSG(name != NULL, "env_int: name is NULL");

    const char *val = getenv(name);
    if (!val || val[0] == '\0') return default_val;

    char *endptr;
    errno = 0;
    long v = strtol(val, &endptr, 10);
    if (*endptr != '\0' || errno == ERANGE || v < 0 || v > ENV_INT_MAX) {
        fprintf(stderr, "warning: %s='%s' invalid, using default %d\n",
                name, val, default_val);
        return default_val;
    }
    return (int)v;
}

/*
 * env_str — Read a string from an environment variable into a buffer.
 *
 * Copies at most buf_size-1 chars. No-op if env var is unset or empty.
 */
static void env_str(const char *name, char *buf, size_t buf_size) {
    ASSERT_MSG(name != NULL, "env_str: name is NULL");
    ASSERT_MSG(buf != NULL, "env_str: buf is NULL");
    ASSERT_MSG(buf_size > 0, "env_str: buf_size is 0");
    const char *val = getenv(name);
    if (val && val[0] != '\0') {
        snprintf(buf, buf_size, "%s", val);
    }
}

/*
 * is_valid_handle — Check handle matches ^[a-zA-Z0-9_-]+$ with bounded length.
 *
 * Iterates at most SIDECAR_MAX_HANDLE characters. Rejects if the string
 * has not terminated within that bound.
 */
static int is_valid_handle(const char *h) {
    if (!h || h[0] == '\0') return 0;
    for (size_t i = 0; i < SIDECAR_MAX_HANDLE; i++) {
        if (h[i] == '\0') return 1;
        if (!isalnum((unsigned char)h[i]) && h[i] != '_' && h[i] != '-')
            return 0;
    }
    return 0; /* too long */
}

int main(int argc, char **argv) {
    sidecar_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* Set defaults from environment */
    env_str("NBS_HANDLE", cfg.handle, sizeof(cfg.handle));
    env_str("NBS_ROOT", cfg.nbs_root, sizeof(cfg.nbs_root));
    env_str("NBS_INITIAL_PROMPT", cfg.initial_prompt, sizeof(cfg.initial_prompt));
    env_str("NBS_REMOTE_HOST", cfg.remote_host, sizeof(cfg.remote_host));
    env_str("NBS_REMOTE_SSH_OPTS", cfg.remote_ssh_opts, sizeof(cfg.remote_ssh_opts));

    cfg.bus_check_interval = env_int("NBS_BUS_CHECK_INTERVAL", 3);
    cfg.notify_cooldown = env_int("NBS_NOTIFY_COOLDOWN", 15);
    cfg.startup_grace = env_int("NBS_STARTUP_GRACE", 30);
    cfg.notify_fail_threshold = env_int("NBS_NOTIFY_FAIL_THRESHOLD", 5);
    cfg.standup_interval = env_int("NBS_STANDUP_INTERVAL", 15);
    cfg.active_heartbeat = env_int("NBS_ACTIVE_HEARTBEAT", 0);
    cfg.flush_interval = env_int("NBS_FLUSH_INTERVAL", 60);
    cfg.poll_interval = env_int("NBS_POLL_INTERVAL", 300);
    cfg.fixup_interval = env_int("NBS_FIXUP_INTERVAL", 3600);

    /* After all env_int calls, timing assertions are deferred until after
     * command-line parsing so that argv overrides are applied first. */

    cfg.is_remote = (cfg.remote_host[0] != '\0') ? 1 : 0;
    cfg.transport_mode = TRANSPORT_TMUX; /* default */

    /* Parse command-line arguments (override env) */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--handle=", 9) == 0) {
            int n = snprintf(cfg.handle, sizeof(cfg.handle), "%s", argv[i] + 9);
            if (n < 0 || (size_t)n >= sizeof(cfg.handle)) {
                fprintf(stderr, "Error: --handle value too long (max %zu)\n",
                        sizeof(cfg.handle) - 1);
                return SIDECAR_EXIT_BAD_ARGS;
            }
        } else if (strncmp(argv[i], "--root=", 7) == 0) {
            int n = snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "%s", argv[i] + 7);
            if (n < 0 || (size_t)n >= sizeof(cfg.nbs_root)) {
                fprintf(stderr, "Error: --root value too long (max %zu)\n",
                        sizeof(cfg.nbs_root) - 1);
                return SIDECAR_EXIT_BAD_ARGS;
            }
        } else if (strncmp(argv[i], "--transport=", 12) == 0) {
            const char *t = argv[i] + 12;
            if (strcmp(t, "tmux") == 0) {
                cfg.transport_mode = TRANSPORT_TMUX;
            } else if (strcmp(t, "pty") == 0) {
                cfg.transport_mode = TRANSPORT_PTY;
            } else {
                fprintf(stderr, "Error: unknown transport '%s' "
                        "(expected 'tmux' or 'pty')\n", t);
                return SIDECAR_EXIT_BAD_ARGS;
            }
        } else if (strncmp(argv[i], "--pane-id=", 10) == 0) {
            int n = snprintf(cfg.pane_id, sizeof(cfg.pane_id), "%s", argv[i] + 10);
            if (n < 0 || (size_t)n >= sizeof(cfg.pane_id)) {
                fprintf(stderr, "Error: --pane-id value too long (max %zu)\n",
                        sizeof(cfg.pane_id) - 1);
                return SIDECAR_EXIT_BAD_ARGS;
            }
        } else if (strncmp(argv[i], "--pty-path=", 11) == 0) {
            int n = snprintf(cfg.pty_session_path, sizeof(cfg.pty_session_path),
                     "%s", argv[i] + 11);
            if (n < 0 || (size_t)n >= sizeof(cfg.pty_session_path)) {
                fprintf(stderr, "Error: --pty-path value too long (max %zu)\n",
                        sizeof(cfg.pty_session_path) - 1);
                return SIDECAR_EXIT_BAD_ARGS;
            }
        } else if (strncmp(argv[i], "--session=", 10) == 0) {
            int n = snprintf(cfg.session_name, sizeof(cfg.session_name),
                     "%s", argv[i] + 10);
            if (n < 0 || (size_t)n >= sizeof(cfg.session_name)) {
                fprintf(stderr, "Error: --session value too long (max %zu)\n",
                        sizeof(cfg.session_name) - 1);
                return SIDECAR_EXIT_BAD_ARGS;
            }
        } else if (strncmp(argv[i], "--initial-prompt=", 17) == 0) {
            int n = snprintf(cfg.initial_prompt, sizeof(cfg.initial_prompt),
                     "%s", argv[i] + 17);
            if (n < 0 || (size_t)n >= sizeof(cfg.initial_prompt)) {
                fprintf(stderr, "Error: --initial-prompt value too long (max %zu)\n",
                        sizeof(cfg.initial_prompt) - 1);
                return SIDECAR_EXIT_BAD_ARGS;
            }
        } else if (strncmp(argv[i], "--log=", 6) == 0) {
            int n = snprintf(cfg.log_file, sizeof(cfg.log_file), "%s", argv[i] + 6);
            if (n < 0 || (size_t)n >= sizeof(cfg.log_file)) {
                fprintf(stderr, "Error: --log value too long (max %zu)\n",
                        sizeof(cfg.log_file) - 1);
                return SIDECAR_EXIT_BAD_ARGS;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return SIDECAR_EXIT_OK;
        } else {
            fprintf(stderr, "Error: unknown argument '%s'\n", argv[i]);
            print_usage();
            return SIDECAR_EXIT_BAD_ARGS;
        }
    }

    /* Config timing assertions — after arg parsing so overrides are applied */
    ASSERT_MSG(cfg.bus_check_interval > 0 && cfg.bus_check_interval < cfg.notify_cooldown,
               "bus_check_interval (%d) must be in (0, notify_cooldown=%d)",
               cfg.bus_check_interval, cfg.notify_cooldown);
    ASSERT_MSG(cfg.startup_grace == 0 || cfg.startup_grace >= cfg.bus_check_interval,
               "startup_grace (%d) must be 0 (disabled) or >= bus_check_interval (%d)",
               cfg.startup_grace, cfg.bus_check_interval);

    /* Validate required fields */
    if (cfg.handle[0] == '\0') {
        fprintf(stderr, "Error: --handle is required\n");
        return SIDECAR_EXIT_BAD_ARGS;
    }
    if (!is_valid_handle(cfg.handle)) {
        fprintf(stderr, "Error: handle '%s' must match ^[a-zA-Z0-9_-]+$\n",
                cfg.handle);
        return SIDECAR_EXIT_BAD_ARGS;
    }
    if (cfg.nbs_root[0] == '\0') {
        fprintf(stderr, "Error: --root is required\n");
        return SIDECAR_EXIT_BAD_ARGS;
    }
    if (cfg.nbs_root[0] != '/') {
        fprintf(stderr, "Error: --root must be an absolute path (got '%s')\n",
                cfg.nbs_root);
        return SIDECAR_EXIT_BAD_ARGS;
    }
    if (access(cfg.nbs_root, F_OK) != 0) {
        fprintf(stderr, "Error: --root directory '%s' does not exist\n",
                cfg.nbs_root);
        return SIDECAR_EXIT_BAD_ARGS;
    }

    /* Transport-specific validation */
    if (cfg.transport_mode == TRANSPORT_TMUX) {
        if (cfg.pane_id[0] == '\0') {
            fprintf(stderr, "Error: --pane-id is required for tmux transport\n");
            return SIDECAR_EXIT_BAD_ARGS;
        }
    } else {
        if (cfg.pty_session_path[0] == '\0') {
            fprintf(stderr, "Error: --pty-path is required for pty transport\n");
            return SIDECAR_EXIT_BAD_ARGS;
        }
        if (cfg.session_name[0] == '\0') {
            fprintf(stderr, "Error: --session is required for pty transport\n");
            return SIDECAR_EXIT_BAD_ARGS;
        }
    }

    /* Build initial prompt: always include handle announcement.
     * If NBS_INITIAL_PROMPT is set, prepend the handle to it.
     * If not set, use the default handle + chat skill prompt. */
    if (cfg.initial_prompt[0] == '\0') {
        int n = snprintf(cfg.initial_prompt, sizeof(cfg.initial_prompt),
                 "Your NBS handle is '%s'. Load /nbs-teams-chat. "
                 "Use this handle for all nbs-chat send commands.",
                 cfg.handle);
        if (n < 0 || (size_t)n >= sizeof(cfg.initial_prompt)) {
            fprintf(stderr, "Error: default initial prompt truncated "
                    "(handle '%s' too long)\n", cfg.handle);
            return SIDECAR_EXIT_BAD_ARGS;
        }
    } else {
        /* Prepend handle announcement to custom initial prompt */
        char tmp[SIDECAR_MAX_PROMPT];
        int n = snprintf(tmp, sizeof(tmp),
                 "Your NBS handle is '%s'. %s",
                 cfg.handle, cfg.initial_prompt);
        if (n >= 0 && (size_t)n < sizeof(tmp)) {
            memcpy(cfg.initial_prompt, tmp, (size_t)n + 1);
        } else {
            fprintf(stderr, "warning: initial prompt too long to prepend handle "
                    "announcement; handle may not be set in session\n");
        }
    }

    /* Redirect stderr to log file if specified */
    if (cfg.log_file[0] != '\0') {
        FILE *logf = freopen(cfg.log_file, "a", stderr);
        if (!logf) {
            fprintf(stdout, "Error: could not open log file '%s', "
                    "and stderr is now closed\n", cfg.log_file);
            return SIDECAR_EXIT_ERROR;
        }
    }

    /* Validate config */
    if (sidecar_config_validate(&cfg) != 0) {
        return SIDECAR_EXIT_BAD_ARGS;
    }

    /* Initialise transport */
    transport_t tp;
    memset(&tp, 0, sizeof(tp));
    int tp_rc;

    if (cfg.transport_mode == TRANSPORT_TMUX) {
        tp_rc = transport_tmux_init(&tp, cfg.pane_id);
    } else {
        tp_rc = transport_pty_init(&tp, cfg.pty_session_path,
                                    cfg.session_name);
    }

    if (tp_rc != 0) {
        fprintf(stderr, "Error: failed to initialise transport\n");
        return SIDECAR_EXIT_ERROR;
    }

    /* Run the sidecar */
    int result = sidecar_run(&cfg, &tp);

    ASSERT_MSG(result == SIDECAR_EXIT_OK || result == SIDECAR_EXIT_ERROR,
               "sidecar_run returned unexpected exit code: %d", result);

    /* Cleanup */
    transport_free(&tp);

    ASSERT_MSG(tp.ctx == NULL,
               "transport_free did not release context");

    return result;
}
