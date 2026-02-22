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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    const char *val = getenv(name);
    if (!val || val[0] == '\0') return default_val;

    char *endptr;
    long v = strtol(val, &endptr, 10);
    if (*endptr != '\0' || v < 0 || v > 100000) {
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
    const char *val = getenv(name);
    if (val && val[0] != '\0') {
        snprintf(buf, buf_size, "%s", val);
    }
}

/*
 * is_valid_handle — Check handle matches ^[a-zA-Z0-9_-]+$.
 */
static int is_valid_handle(const char *h) {
    if (!h || h[0] == '\0') return 0;
    for (const char *p = h; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-')
            return 0;
    }
    return 1;
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

    cfg.is_remote = (cfg.remote_host[0] != '\0') ? 1 : 0;
    cfg.transport_mode = TRANSPORT_TMUX; /* default */

    /* Parse command-line arguments (override env) */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--handle=", 9) == 0) {
            snprintf(cfg.handle, sizeof(cfg.handle), "%s", argv[i] + 9);
        } else if (strncmp(argv[i], "--root=", 7) == 0) {
            snprintf(cfg.nbs_root, sizeof(cfg.nbs_root), "%s", argv[i] + 7);
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
            snprintf(cfg.pane_id, sizeof(cfg.pane_id), "%s", argv[i] + 10);
        } else if (strncmp(argv[i], "--pty-path=", 11) == 0) {
            snprintf(cfg.pty_session_path, sizeof(cfg.pty_session_path),
                     "%s", argv[i] + 11);
        } else if (strncmp(argv[i], "--session=", 10) == 0) {
            snprintf(cfg.session_name, sizeof(cfg.session_name),
                     "%s", argv[i] + 10);
        } else if (strncmp(argv[i], "--initial-prompt=", 17) == 0) {
            snprintf(cfg.initial_prompt, sizeof(cfg.initial_prompt),
                     "%s", argv[i] + 17);
        } else if (strncmp(argv[i], "--log=", 6) == 0) {
            snprintf(cfg.log_file, sizeof(cfg.log_file), "%s", argv[i] + 6);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return SIDECAR_EXIT_OK;
        } else {
            fprintf(stderr, "Error: unknown argument '%s'\n", argv[i]);
            print_usage();
            return SIDECAR_EXIT_BAD_ARGS;
        }
    }

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

    /* Build default initial prompt if not set */
    if (cfg.initial_prompt[0] == '\0') {
        snprintf(cfg.initial_prompt, sizeof(cfg.initial_prompt),
                 "Your NBS handle is '%s'. Load /nbs-teams-chat. "
                 "Use this handle for all nbs-chat send commands.",
                 cfg.handle);
    }

    /* Redirect stderr to log file if specified */
    if (cfg.log_file[0] != '\0') {
        FILE *logf = freopen(cfg.log_file, "a", stderr);
        if (!logf) {
            fprintf(stdout, "warning: could not open log file '%s'\n",
                    cfg.log_file);
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

    /* Cleanup */
    transport_free(&tp);

    return result;
}
