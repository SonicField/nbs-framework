/*
 * main.c — nbs-sidecar entry point.
 *
 * Parses command-line arguments and environment variables into a
 * sidecar_config_t, initialises the transport, and calls sidecar_run().
 *
 * Usage:
 *   nbs-sidecar --handle=NAME --root=PATH --session=HANDLE
 *
 * All parameters can also be set via NBS_* environment variables.
 * Command-line arguments take precedence over environment variables.
 */

#include "sidecar.h"
#include "transport.h"
#include "../nbs-common/nbs_assert.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* No timing interval should exceed ~27 hours (100000 seconds).
 * This bounds env_int values to a sane range for all config fields. */
#define ENV_INT_MAX 100000

/*
 * sanitise_for_display — Copy src to dst, replacing non-printable and
 * non-ASCII characters with '?'. Prevents terminal escape injection
 * when printing untrusted input (e.g., handle from argv) to stderr.
 *
 * Preconditions: dst_size > 0, dst != NULL, src != NULL.
 * Postcondition: dst is NUL-terminated, length <= dst_size-1.
 */
static void sanitise_for_display(char *dst, size_t dst_size, const char *src) {
    ASSERT_MSG(dst != NULL, "sanitise_for_display: dst is NULL");
    ASSERT_MSG(src != NULL, "sanitise_for_display: src is NULL");
    ASSERT_MSG(dst_size > 0, "sanitise_for_display: dst_size is 0");

    size_t i;
    for (i = 0; i < dst_size - 1 && src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    dst[i] = '\0';
}

static void print_usage(void) {
    fprintf(stderr,
        "nbs-sidecar: Claude Code session monitor\n\n"
        "Usage:\n"
        "  nbs-sidecar --handle=NAME --root=PATH --session=HANDLE\n\n"
        "Options:\n"
        "  --handle=NAME          Agent handle (required)\n"
        "  --root=PATH            Project root containing .nbs/ (required)\n"
        "  --transport=ts         Transport mode (default: ts)\n"
        "  --session=HANDLE       nbs-ts session handle (required)\n"
        "  --initial-prompt=TEXT  Custom initial prompt\n"
        "  --log=PATH             Log file for sidecar stderr\n\n"
        "Environment (defaults, overridden by args):\n"
        "  NBS_HANDLE, NBS_ROOT, NBS_BUS_CHECK_INTERVAL (3),\n"
        "  NBS_NOTIFY_COOLDOWN (15), NBS_STARTUP_GRACE (30),\n"
        "  NBS_NOTIFY_FAIL_THRESHOLD (5), NBS_FLUSH_INTERVAL (60),\n"
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
    /* H4 fix: reject 0 when the default is positive. Fields like
     * bus_check_interval and notify_fail_threshold must be > 0; accepting
     * 0 from the environment would cause division-by-zero or infinite loops.
     * Fields where 0 is valid (poll_interval, flush_interval) have default 0. */
    if (v == 0 && default_val > 0) {
        fprintf(stderr, "warning: %s='0' invalid (must be > 0), using default %d\n",
                name, default_val);
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
    cfg.flush_interval = env_int("NBS_FLUSH_INTERVAL", 60);
    /* Disabled by default — /nbs-notify handles event delivery directly.
     * /nbs-poll was a legacy safety net that burned context tokens every 5
     * minutes and blindly ack'd bus events. Re-enable with NBS_POLL_INTERVAL=300
     * if notification path proves unreliable. */
    cfg.poll_interval = env_int("NBS_POLL_INTERVAL", 0);
    /* Oracle intervals — all in SECONDS. Prime numbers prevent
     * oracles from firing simultaneously and creating chat storms.
     * Env vars are also in seconds for consistency. */
    cfg.fixup_interval = env_int("NBS_FIXUP_INTERVAL", 61 * 60);
    cfg.librarian_interval = env_int("NBS_LIBRARIAN_INTERVAL", 23 * 60);
    cfg.pythia_interval = env_int("NBS_PYTHIA_INTERVAL", 37 * 60);
    cfg.shepard_interval = env_int("NBS_SHEPARD_INTERVAL", 31 * 60);

    /* After all env_int calls, timing assertions are deferred until after
     * command-line parsing so that argv overrides are applied first. */

    cfg.is_remote = (cfg.remote_host[0] != '\0') ? 1 : 0;
    cfg.transport_mode = TRANSPORT_TS; /* default */

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
            if (strcmp(t, "ts") == 0) {
                cfg.transport_mode = TRANSPORT_TS;
            } else {
                fprintf(stderr, "Error: unknown transport '%s' "
                        "(expected 'ts')\n", t);
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

    /* Range assertions on oracle interval fields.
     * All intervals are in seconds. env_int caps at ENV_INT_MAX=100000. */
    ASSERT_MSG(cfg.fixup_interval >= 0,
               "fixup_interval out of range: %d", cfg.fixup_interval);
    ASSERT_MSG(cfg.librarian_interval >= 0,
               "librarian_interval out of range: %d", cfg.librarian_interval);
    ASSERT_MSG(cfg.pythia_interval >= 0,
               "pythia_interval out of range: %d", cfg.pythia_interval);
    ASSERT_MSG(cfg.shepard_interval >= 0,
               "shepard_interval out of range: %d", cfg.shepard_interval);

    /* Validate required fields */
    if (cfg.handle[0] == '\0') {
        fprintf(stderr, "Error: --handle is required\n");
        return SIDECAR_EXIT_BAD_ARGS;
    }
    if (!is_valid_handle(cfg.handle)) {
        char safe_handle[SIDECAR_MAX_HANDLE];
        sanitise_for_display(safe_handle, sizeof(safe_handle), cfg.handle);
        fprintf(stderr, "Error: handle '%s' must match ^[a-zA-Z0-9_-]+$\n",
                safe_handle);
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
    if (cfg.session_name[0] == '\0') {
        fprintf(stderr, "Error: --session is required (nbs-ts session handle)\n");
        return SIDECAR_EXIT_BAD_ARGS;
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

    /* Ignore SIGHUP — the sidecar is backgrounded by nbs-claude and must
     * survive parent shell exit and session restarts. Without this,
     * any session kill or shell exit sends SIGHUP and silently kills the
     * sidecar with no log output (the signal arrives before any stderr
     * write can complete). */
    {
        void (*prev_hup)(int) = signal(SIGHUP, SIG_IGN);
        ASSERT_MSG(prev_hup != SIG_ERR,
                   "signal(SIGHUP, SIG_IGN) failed: %s", strerror(errno));
        void (*prev_pipe)(int) = signal(SIGPIPE, SIG_IGN);
        ASSERT_MSG(prev_pipe != SIG_ERR,
                   "signal(SIGPIPE, SIG_IGN) failed: %s", strerror(errno));
    }

    /* Redirect stderr to log file if specified */
    if (cfg.log_file[0] != '\0') {
        FILE *logf = freopen(cfg.log_file, "a", stderr);
        if (!logf) {
            /* After failed freopen, stderr is in an indeterminate state
             * (ISO C 7.21.5.4). Write to stdout as a last resort. */
            fprintf(stdout, "Error: could not open log file '%s', "
                    "and stderr is now in indeterminate state\n", cfg.log_file);
            return SIDECAR_EXIT_ERROR;
        }
        ASSERT_MSG(logf == stderr,
                   "freopen returned non-stderr FILE* for log redirect");
        /* freopen to a file switches stderr from unbuffered to fully
         * buffered. The sidecar runs indefinitely, so the buffer would
         * never flush. Set line-buffered so each fprintf+newline is
         * visible immediately in the log file. */
        setvbuf(stderr, NULL, _IOLBF, 0);
    }

    /* Validate config — sidecar_config_validate logs each failing field
     * to stderr. Add a summary line so the exit path is never silent. */
    if (sidecar_config_validate(&cfg) != 0) {
        fprintf(stderr, "Error: config validation failed "
                "(see per-field errors above)\n");
        return SIDECAR_EXIT_BAD_ARGS;
    }

    /* Initialise transport */
    transport_t tp;
    memset(&tp, 0, sizeof(tp));
    int tp_rc;

    tp_rc = transport_ts_init(&tp, cfg.session_name);

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
