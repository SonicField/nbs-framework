/*
 * sidecar.h — nbs-sidecar configuration and state definitions
 *
 * The sidecar monitors a Claude Code session (via tmux or pty-session)
 * and injects notifications, handles dialogues, and triggers periodic
 * coordination actions.
 */

#ifndef NBS_SIDECAR_H
#define NBS_SIDECAR_H

#include "../nbs-common/nbs_assert.h"

#include <stdint.h>
#include <time.h>

/* Forward declaration */
typedef struct transport transport_t;

/* Maximum sizes */
#define SIDECAR_MAX_PATH       4096
#define SIDECAR_MAX_HANDLE      64
#define SIDECAR_MAX_CONTENT   32768   /* Max pane capture content */
#define SIDECAR_MAX_MESSAGE     256   /* Max notify message length */
#define SIDECAR_MAX_PROMPT     4096   /* Max initial prompt length */

/* Exit codes */
#define SIDECAR_EXIT_OK         0
#define SIDECAR_EXIT_ERROR      1
#define SIDECAR_EXIT_BAD_ARGS   4

/* Transport mode */
typedef enum {
    TRANSPORT_TMUX,
    TRANSPORT_PTY
} transport_mode_t;

/*
 * sidecar_config_t — All configurable parameters.
 *
 * Invariants:
 *   - handle is NUL-terminated, matches ^[a-zA-Z0-9_-]+$
 *   - nbs_root is an absolute path to an existing directory
 *   - bus_check_interval > 0
 *   - notify_cooldown >= 0
 *   - startup_grace >= 0
 *   - notify_fail_threshold > 0
 *   - All paths are NUL-terminated, length < SIDECAR_MAX_PATH
 */
typedef struct {
    char handle[SIDECAR_MAX_HANDLE];
    char nbs_root[SIDECAR_MAX_PATH];
    char pty_session_path[SIDECAR_MAX_PATH];
    char session_name[256];
    char pane_id[64];
    char initial_prompt[SIDECAR_MAX_PROMPT];
    char log_file[SIDECAR_MAX_PATH];

    int bus_check_interval;     /* seconds between bus/chat checks */
    int notify_cooldown;        /* min seconds between notifications */
    int startup_grace;          /* seconds before allowing notifications */
    int notify_fail_threshold;  /* failures before self-heal */
    int standup_interval;       /* minutes between standups (0=disabled) */
    int active_heartbeat;       /* seconds between heartbeats (0=disabled) */
    int flush_interval;         /* seconds between bare Enter flushes (0=disabled) */
    int poll_interval;          /* seconds between /nbs-poll injections (0=disabled) */
    int fixup_interval;         /* seconds between auto-fixup runs (0=disabled) */
    int librarian_interval;     /* seconds between librarian checks (0=disabled) */

    int is_remote;
    char remote_host[256];
    char remote_ssh_opts[512];

    transport_mode_t transport_mode;
} sidecar_config_t;

/*
 * sidecar_state_t — Mutable runtime state.
 *
 * Invariants:
 *   - idle_seconds >= 0
 *   - bus_check_counter >= 0
 *   - notify_fail_count >= 0
 *   - sidecar_start_time > 0 after initial prompt injection
 */
typedef struct {
    int idle_seconds;
    int bus_check_counter;
    uint64_t last_content_hash;
    time_t sidecar_start_time;
    time_t last_notify_time;
    time_t last_standup_time;
    time_t last_heartbeat_time;
    time_t last_flush_time;
    time_t last_poll_time;
    time_t last_fixup_check;
    time_t last_librarian_check;
    int notify_fail_count;
    int pythia_last_trigger_count;
    int shepard_last_trigger_count;
    int mention_detected;
    char mention_payload[SIDECAR_MAX_MESSAGE];

    /* Bus check results */
    int bus_event_count;
    char bus_max_priority[16];
    char bus_event_summary[SIDECAR_MAX_MESSAGE];
    int chat_unread_count;
    char chat_unread_summary[SIDECAR_MAX_MESSAGE];
    char notify_message[SIDECAR_MAX_MESSAGE];

    /* Registry state */
    int control_inbox_line;

    /* Deferred ack retry counters */
    int query_retry_count;
    int interrupt_retry_count;
    int mention_retry_count;
} sidecar_state_t;

/*
 * sidecar_run — Main sidecar loop. Does not return until the
 * monitored pane/session exits or a fatal error occurs.
 *
 * Preconditions:
 *   - cfg is fully validated (sidecar_config_validate() returned 0)
 *   - tp is initialised for cfg->transport_mode
 *
 * Postconditions:
 *   - Returns 0 on clean exit, 1 on error
 */
int sidecar_run(const sidecar_config_t *cfg, transport_t *tp);

/*
 * sidecar_config_validate — Validate all config fields.
 *
 * Returns 0 if valid, -1 with error message on stderr if invalid.
 */
int sidecar_config_validate(const sidecar_config_t *cfg);

#endif /* NBS_SIDECAR_H */
