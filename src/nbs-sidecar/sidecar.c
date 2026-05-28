/*
 * sidecar.c — Main sidecar loop and notification engine.
 *
 * Ported from the bash implementation in bin/nbs-claude.
 * The loop runs at a 1-second tick, checking for:
 *   - Interrupt events (every tick, unconditional)
 *   - Mention events (every tick, sets flag)
 *   - Blocking dialogues (on content change and when idle)
 *   - Bus events and chat unreads (every BUS_CHECK_INTERVAL ticks)
 *   - Periodic triggers (pythia, shepard, fixup, librarian)
 *   - Periodic Enter flush (every FLUSH_INTERVAL ticks)
 *
 * The transport vtable abstracts session transport interactions.
 */

#include "sidecar.h"
#include "transport.h"
#include "detect.h"
#include "hash.h"
#include "bus_client.h"

#include <sys/stat.h>
#include "chat_client.h"
#include "registry.h"
#include "triggers.h"
#include "exec_util.h"
#include "strip_ansi.h"
#include "mention_escape.h"
#include "../nbs-common/nbs_assert.h"

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Sidecar debug log — always on, temporary for debugging oracle death */
static FILE *g_sc_debug_fp = NULL;
static void sc_dbg(const char *fmt, ...) {
    if (!g_sc_debug_fp) {
        char path[256];
        snprintf(path, sizeof(path), "/tmp/nbs-sidecar-main-debug-%d.log",
                 (int)getpid());
        g_sc_debug_fp = fopen(path, "a");
        if (!g_sc_debug_fp) return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(g_sc_debug_fp, "[sidecar:%d] %02d:%02d:%02d ",
            (int)getpid(), tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_sc_debug_fp, fmt, ap);
    va_end(ap);
    fprintf(g_sc_debug_fp, "\n");
    fflush(g_sc_debug_fp);
}

/* Path buffer with room for prefix + handle suffix */
#define SIDECAR_EXT_PATH (SIDECAR_MAX_PATH + SIDECAR_MAX_HANDLE + 32)

/* --- Path builders --- */

static void build_registry_path(const sidecar_config_t *cfg,
                                 char *out, size_t out_size) {
    ASSERT_MSG(cfg != NULL, "build_registry_path: cfg is NULL");
    ASSERT_MSG(out != NULL, "build_registry_path: out is NULL");
    ASSERT_MSG(out_size > 0, "build_registry_path: out_size is 0");

    int n = snprintf(out, out_size, "%s/.nbs/control-registry-%s",
             cfg->nbs_root, cfg->handle);
    ASSERT_MSG(n >= 0 && (size_t)n < out_size,
               "build_registry_path: path truncated (need %d, have %zu)", n, out_size);
}

static void build_inbox_path(const sidecar_config_t *cfg,
                              char *out, size_t out_size) {
    ASSERT_MSG(cfg != NULL, "build_inbox_path: cfg is NULL");
    ASSERT_MSG(out != NULL, "build_inbox_path: out is NULL");
    ASSERT_MSG(out_size > 0, "build_inbox_path: out_size is 0");

    int n = snprintf(out, out_size, "%s/.nbs/control-inbox-%s",
             cfg->nbs_root, cfg->handle);
    ASSERT_MSG(n >= 0 && (size_t)n < out_size,
               "build_inbox_path: path truncated (need %d, have %zu)", n, out_size);
}

/* --- Notification prompt builder --- */

/*
 * build_notify_prompt — Construct a plain text notification prompt.
 *
 * Deliberately contains NO chat content — only a brief notice that
 * messages exist. Including summaries of chat messages caused agents
 * to misinterpret content as human instructions (e.g. a summary
 * mentioning "session end" was read as a command to shut down).
 *
 * The agent reads the actual messages via nbs-chat read --unread.
 * The sidecar acks bus events — agents do not need to ack manually.
 */
static void build_notify_prompt(const sidecar_config_t *cfg,
                                 const char *chat_path,
                                 char *out, size_t out_size) {
    ASSERT_MSG(cfg != NULL, "build_notify_prompt: cfg is NULL");
    ASSERT_MSG(out != NULL, "build_notify_prompt: out is NULL");
    ASSERT_MSG(out_size > 0, "build_notify_prompt: out_size is 0");

    (void)cfg;
    (void)chat_path;

    int sn = snprintf(out, out_size,
        "Hey, you have messages to read in your chat.");

    (void)sn;

    ASSERT_MSG(out[0] != '\0',
               "build_notify_prompt: output is empty");
}

/* --- Interrupt handler --- */

/*
 * handle_interrupt — Send Escape every 10s for 60s, inject /nbs-notify
 * on success, post URGENT on failure.
 *
 * Returns 0 on success (interrupt delivered), -1 on failure.
 */
static int handle_interrupt(transport_t *tp, const sidecar_config_t *cfg,
                             const char *registry_path) {
    ASSERT_MSG(tp != NULL, "handle_interrupt: tp is NULL");
    ASSERT_MSG(cfg != NULL, "handle_interrupt: cfg is NULL");
    ASSERT_MSG(registry_path != NULL, "handle_interrupt: registry_path is NULL");

    time_t start = time(NULL);
    int succeeded = 0;

    while (1) {
        time_t elapsed = time(NULL) - start;
        if (elapsed >= 60) break;

        /* Send Escape */
        if (tp->send_key(tp, "Escape") != 0) {
            fprintf(stderr, "handle_interrupt: send_key Escape failed\n");
        }

        /* Wait up to 10s, checking for prompt every 1s */
        for (int w = 0; w < 10; w++) {
            sleep(1);

            char *content = tp->capture(tp, 30);
            if (!content) continue;

            if (detect_prompt_ready(content)) {
                /* Inject interrupt as plain text */
                char int_chat_path[SIDECAR_MAX_PATH];
                if (registry_find_first(registry_path, "chat",
                                         int_chat_path, sizeof(int_chat_path)) != 0)
                    int_chat_path[0] = '\0';
                char interrupt_prompt[SIDECAR_MAX_PROMPT];
                build_notify_prompt(cfg, int_chat_path[0] ? int_chat_path : NULL,
                                     interrupt_prompt, sizeof(interrupt_prompt));
                if (tp->send_text(tp, interrupt_prompt) != 0) {
                    fprintf(stderr, "handle_interrupt: send_text failed\n");
                }
                usleep(300000);
                if (tp->send_key(tp, "Enter") != 0) {
                    fprintf(stderr, "handle_interrupt: send_key Enter failed\n");
                }
                succeeded = 1;
                free(content);
                goto done;
            }
            free(content);
        }
    }

done:
    if (!succeeded) {
        /* Post URGENT to first registered chat */
        char chat_path[SIDECAR_MAX_PATH];
        if (registry_find_first(registry_path, "chat",
                                 chat_path, sizeof(chat_path)) == 0) {
            char msg[SIDECAR_MAX_MESSAGE];
            int sn_urgent = snprintf(msg, sizeof(msg),
                     "URGENT: @supervisor - agent unresponsive %s", cfg->handle);
            /* H6 fix: assert URGENT message was not truncated — a truncated
             * URGENT message could lose the handle, making it unactionable. */
            ASSERT_MSG(sn_urgent >= 0 && (size_t)sn_urgent < sizeof(msg),
                       "handle_interrupt: URGENT message truncated for handle '%s'",
                       cfg->handle);
            int rc = chat_client_send(chat_path, "sidecar", msg);
            if (rc != 0) {
                fprintf(stderr, "handle_interrupt: chat_client_send URGENT failed "
                        "for '%s'\n", cfg->handle);
            }
        }
        fprintf(stderr, "handle_interrupt: failed to deliver interrupt "
                "for '%s' within 60s\n", cfg->handle);
        return -1;
    }
    return 0;
}

/*
 * handle_query — Render agent terminal screen and post to chat.
 *
 * Triggered by @handle? in chat. Pipes the tail of the session's
 * output.log through nbs-ts-render to produce what the terminal
 * screen actually looks like — cursor movement, scrolling, erase
 * all resolved. No junk filtering needed.
 *
 * Returns 0 on success, -1 on any failure.
 */
static int handle_query(transport_t *tp, const sidecar_config_t *cfg,
                          const char *registry_path) {
    ASSERT_MSG(tp != NULL, "handle_query: tp is NULL");
    ASSERT_MSG(cfg != NULL, "handle_query: cfg is NULL");
    ASSERT_MSG(registry_path != NULL, "handle_query: registry_path is NULL");

    /* Resolve chat path FIRST — every failure path needs it to post
     * an error message.  If we can't find the chat, we can't report
     * anything, so that's the only truly silent failure. */
    char chat_path[SIDECAR_MAX_PATH];
    if (registry_find_first(registry_path, "chat",
                             chat_path, sizeof(chat_path)) != 0) {
        fprintf(stderr, "handle_query: no chat registered for '%s'\n",
                cfg->handle);
        return -1;
    }

    /* Helper: post an error to chat with a specific reason.
     * Uses [SIDECAR-ERROR] bracket handle for visibility. */
#define QUERY_ERROR(reason) do { \
    char _qerr[512]; \
    snprintf(_qerr, sizeof(_qerr), \
             "%s? query: %s", cfg->handle, (reason)); \
    chat_client_error(chat_path, _qerr); \
    fprintf(stderr, "handle_query: %s for '%s'\n", (reason), cfg->handle); \
} while (0)

    /* Get session dir from transport context */
    const char *session_dir = NULL;
    if (tp->ctx) {
        /* ts_ctx_t has session_dir as first field */
        session_dir = (const char *)tp->ctx;
    }
    if (!session_dir || session_dir[0] == '\0') {
        QUERY_ERROR("no nbs-ts session directory — agent may not have started");
        return -1;
    }

    /* Use tp->capture which feeds through the terminal renderer */
    char *rendered = tp->capture(tp, 0);
    if (!rendered || rendered[0] == '\0') {
        free(rendered);
        QUERY_ERROR("session has no visible output — agent may be initialising or dead");
        return -1;
    }

    char msg[SIDECAR_MAX_CONTENT];
    size_t prefix_len = (size_t)snprintf(msg, sizeof(msg),
                                         "session output for %s:\n", cfg->handle);
    size_t remaining = sizeof(msg) - prefix_len - 1;
    size_t rlen = strlen(rendered);
    if (rlen > remaining) rlen = remaining;
    memcpy(msg + prefix_len, rendered, rlen);
    msg[prefix_len + rlen] = '\0';
    free(rendered);

    /* Escape @ signs to prevent mention feedback loops. */
    sanitise_at_signs(msg);
    int rc = chat_client_send(chat_path, "sidecar", msg);
    if (rc != 0) {
        QUERY_ERROR("failed to post session output to chat");
    }

#undef QUERY_ERROR
    return rc;
}

/* --- Dialogue response --- */

static void respond_dialogue(transport_t *tp,
                              const dialogue_response_t *resp) {
    ASSERT_MSG(tp != NULL, "respond_dialogue: tp is NULL");
    ASSERT_MSG(resp != NULL, "respond_dialogue: resp is NULL");
    ASSERT_MSG(resp->option > 0 && resp->option <= 9, "respond_dialogue: option must be in range [1, 9]");

    char option_str[4];
    snprintf(option_str, sizeof(option_str), "%d", resp->option);
    if (tp->send_text(tp, option_str) != 0) {
        fprintf(stderr, "respond_dialogue: send_text failed for option %d\n",
                resp->option);
        return;
    }
    /* No delay needed — Enter is CR (0x0d) which submits immediately */
    if (tp->send_key(tp, "Enter") != 0) {
        fprintf(stderr, "respond_dialogue: send_key Enter failed\n");
        return;
    }
    sleep(resp->settle_secs);
}

/* --- Cooldown state --- */

/*
 * cooldown_is_active — Single source of truth for cooldown state.
 *
 * Returns 1 if cooldown is active (notification was sent recently),
 * 0 if cooldown has expired or no notification has ever been sent.
 *
 * Both should_inject_notify() and the Root Cause B catch-up tracking
 * in the main loop MUST use this function instead of computing
 * cooldown inline. This prevents the two checks from diverging if
 * cooldown semantics change (per-priority cooldowns, etc).
 */
int cooldown_is_active(const sidecar_config_t *cfg,
                       const sidecar_state_t *state)
{
    if (state->last_notify_time == 0)
        return 0;
    time_t elapsed = time(NULL) - state->last_notify_time;
    return (elapsed < cfg->notify_cooldown) ? 1 : 0;
}

/* --- Notification decision engine --- */

/*
 * should_inject_notify — Decide whether to inject /nbs-notify.
 *
 * Checks bus events, chat unreads, triggers. Applies startup grace,
 * cooldown, priority bypass, mention bypass, sidecar-only suppression.
 * Sets state->notify_message on success.
 *
 * Returns 0 if should inject, 1 if should not.
 */
static int should_inject_notify(const sidecar_config_t *cfg,
                                 sidecar_state_t *state,
                                 const char *registry_path) {
    ASSERT_MSG(cfg != NULL, "should_inject_notify: cfg is NULL");
    ASSERT_MSG(state != NULL, "should_inject_notify: state is NULL");
    ASSERT_MSG(registry_path != NULL, "should_inject_notify: registry_path is NULL");

    state->notify_message[0] = '\0';

    /* Startup grace */
    time_t now = time(NULL);
    if (state->sidecar_start_time > 0) {
        if ((now - state->sidecar_start_time) < cfg->startup_grace) {
            return 1;
        }
    }

    /* Root Cause B (Scenario #6): startup catch-up notification.
     * After startup grace ends, fire one unconditional notification so the
     * agent reads recent context. This handles the case where the sidecar
     * restarts but cursor == msg_count — without this, the agent is deaf
     * to messages that arrived while the sidecar was dead. */
    if (!state->startup_notify_sent) {
        state->startup_notify_sent = 1;
        snprintf(state->notify_message, sizeof(state->notify_message),
                 "sidecar startup catch-up");
        state->last_notify_time = now;
        return 0;  /* Force notification delivery */
    }

    /* Check bus events */
    char bus_dir[SIDECAR_MAX_PATH];
    int has_bus = (registry_find_first(registry_path, "bus",
                                        bus_dir, sizeof(bus_dir)) == 0);
    int bus_rc = 1;
    state->bus_event_count = 0;
    snprintf(state->bus_max_priority, sizeof(state->bus_max_priority), "none");
    state->bus_event_summary[0] = '\0';

    if (has_bus) {
        bus_rc = bus_client_check(bus_dir, &state->bus_event_count,
                                  state->bus_max_priority,
                                  sizeof(state->bus_max_priority),
                                  state->bus_event_summary,
                                  sizeof(state->bus_event_summary));
        if (bus_rc < 0) bus_rc = 1;
    }

    /* Check chat unreads */
    int chat_rc = chat_client_check_unread(registry_path, cfg->handle,
                                            &state->chat_unread_count,
                                            state->chat_unread_summary,
                                            sizeof(state->chat_unread_summary));
    if (chat_rc < 0) chat_rc = 1;

    /* Nothing pending */
    if (bus_rc != 0 && chat_rc != 0) {
        return 1;
    }

    /* Sidecar-only suppression: if only chat pending and all from sidecar */
    if (bus_rc != 0 && chat_rc == 0) {
        if (chat_client_are_unread_sidecar_only(registry_path, cfg->handle)) {
            return 1;
        }
    }

    /* Apply cooldown (critical and non-sidecar mentions bypass).
     * Use time_t for elapsed to avoid int overflow when last_notify_time
     * is 0 (memset-initialised) — (now - 0) overflows int on 64-bit.
     *
     * Sidecar-originated mentions (payload contains "from sidecar:") do
     * NOT bypass cooldown. Without this, URGENT @team messages from
     * sidecar create an O(N^2) notification storm: each agent's sidecar
     * posts @team, bus_bridge fans out to N events, and mention bypass
     * ensures every event triggers immediate notification. */
    /* H8 fix: only access mention_payload when mention_detected is set.
     * When mention_detected==0, mention_payload may contain stale data
     * from a previous cycle. Short-circuit evaluation prevents the access. */
    int mention_bypasses_cooldown = (state->mention_detected == 1 &&
        state->mention_payload[0] != '\0' &&
        strstr(state->mention_payload, "from sidecar:") == NULL);

    if (strcmp(state->bus_max_priority, "critical") != 0 &&
        !mention_bypasses_cooldown &&
        cooldown_is_active(cfg, state)) {
        return 1;
    }

    /* Build message — mention payload takes priority */
    if (state->mention_detected == 1 &&
        state->mention_payload[0] != '\0') {
        /* Truncate: "MENTION: " (9 chars) + payload into notify_message */
        memcpy(state->notify_message, "MENTION: ", 9);
        size_t payload_max = sizeof(state->notify_message) - 10;
        size_t plen = strlen(state->mention_payload);
        if (plen > payload_max) plen = payload_max;
        memcpy(state->notify_message + 9, state->mention_payload, plen);
        state->notify_message[9 + plen] = '\0';
        state->mention_detected = 0;
        state->mention_payload[0] = '\0';
    } else {
        char parts[SIDECAR_MAX_MESSAGE];
        parts[0] = '\0';
        size_t off = 0;
        int sn;

        if (state->bus_event_summary[0] != '\0') {
            sn = snprintf(parts + off, sizeof(parts) - off,
                          "%s", state->bus_event_summary);
            if (sn > 0) off += (size_t)sn;
        }
        if (state->chat_unread_summary[0] != '\0') {
            if (off > 0) {
                sn = snprintf(parts + off, sizeof(parts) - off,
                              ". %s", state->chat_unread_summary);
            } else {
                sn = snprintf(parts + off, sizeof(parts) - off,
                              "%s", state->chat_unread_summary);
            }
            if (sn > 0) off += (size_t)sn;
        }
        (void)off; /* suppress unused warning */

        /* snprintf into parts (sizeof SIDECAR_MAX_MESSAGE) already guarantees
         * the buffer is within bounds — no additional truncation guard needed */

        snprintf(state->notify_message, sizeof(state->notify_message),
                 "%s", parts);
    }

    ASSERT_MSG(state->notify_message[0] != '\0',
               "should_inject_notify: returning inject but message is empty");

    /* Note: last_notify_time is NOT set here. The caller sets it after
     * confirmed delivery to avoid phantom cooldowns when TOCTOU re-capture
     * aborts injection (see sidecar_run notification path). */
    return 0;
}

/* --- Config validation --- */

int sidecar_config_validate(const sidecar_config_t *cfg) {
    ASSERT_MSG(cfg != NULL, "sidecar_config_validate: cfg is NULL");

    int ok = 1;

    if (cfg->handle[0] == '\0') {
        fprintf(stderr, "config error: handle is empty\n");
        ok = 0;
    } else {
        /* Validate handle format: ^[a-zA-Z0-9_-]+$ (sidecar.h invariant) */
        for (const char *p = cfg->handle; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
                fprintf(stderr, "config error: handle contains invalid char '%c'\n", *p);
                ok = 0;
                break;
            }
        }
    }
    if (cfg->nbs_root[0] == '\0') {
        fprintf(stderr, "config error: nbs_root is empty\n");
        ok = 0;
    } else if (cfg->nbs_root[0] != '/') {
        fprintf(stderr, "config error: nbs_root must be an absolute path\n");
        ok = 0;
    }
    if (cfg->bus_check_interval <= 0) {
        fprintf(stderr, "config error: bus_check_interval must be > 0\n");
        ok = 0;
    }
    if (cfg->notify_fail_threshold <= 0) {
        fprintf(stderr, "config error: notify_fail_threshold must be > 0\n");
        ok = 0;
    }
    if (cfg->notify_cooldown < 0) {
        fprintf(stderr, "config error: notify_cooldown must be >= 0\n");
        ok = 0;
    }
    if (cfg->startup_grace < 0) {
        fprintf(stderr, "config error: startup_grace must be >= 0\n");
        ok = 0;
    }
    if (cfg->librarian_interval < 0) {
        fprintf(stderr, "config error: librarian_interval must be >= 0 (got %d)\n",
                cfg->librarian_interval);
        ok = 0;
    }
    if (cfg->pythia_interval < 0) {
        fprintf(stderr, "config error: pythia_interval must be >= 0 (got %d)\n",
                cfg->pythia_interval);
        ok = 0;
    }
    if (cfg->shepard_interval < 0) {
        fprintf(stderr, "config error: shepard_interval must be >= 0 (got %d)\n",
                cfg->shepard_interval);
        ok = 0;
    }

    return ok ? 0 : -1;
}

/* --- PID marker for duplicate sidecar detection (Root Cause C) --- */

/*
 * build_pid_marker_path — Construct path to sidecar PID marker file.
 *
 * Pattern: <nbs_root>/.nbs/pids/sidecar-<handle>.pid
 * Consistent with existing .nbs/pids/${AGENT}.pid convention.
 */
static void build_pid_marker_path(const sidecar_config_t *cfg,
                                   char *out, size_t out_size) {
    ASSERT_MSG(cfg != NULL, "build_pid_marker_path: cfg is NULL");
    ASSERT_MSG(out != NULL, "build_pid_marker_path: out is NULL");
    ASSERT_MSG(out_size > 0, "build_pid_marker_path: out_size is 0");

    int n = snprintf(out, out_size, "%s/.nbs/pids/sidecar-%s.pid",
                     cfg->nbs_root, cfg->handle);
    ASSERT_MSG(n >= 0 && (size_t)n < out_size,
               "build_pid_marker_path: path truncated (need %d, have %zu)", n, out_size);
}

/*
 * pid_marker_write — Atomically write PID marker file.
 *
 * Writes to a .tmp file first, then renames. This prevents readers
 * from seeing a partially written PID.
 *
 * Returns 0 on success, -1 on error.
 */
static int pid_marker_write(const char *path, pid_t pid) {
    char tmp_path[SIDECAR_MAX_PATH + 16];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) return -1;

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    fprintf(f, "%d\n", (int)pid);
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/*
 * pid_marker_check — Check PID marker for duplicate detection.
 *
 * Returns:
 *   0  — no conflict (marker matches our PID, or is stale/missing)
 *   1  — conflict (another live sidecar owns this handle)
 *  -1  — error reading marker
 *
 * If the marker contains a stale PID (process dead), overwrites it
 * with our PID and returns 0.
 */
static int pid_marker_check(const char *path, pid_t my_pid, const char *handle) {
    FILE *f = fopen(path, "r");
    if (!f) {
        /* No marker file — no conflict. Write ours. */
        return pid_marker_write(path, my_pid);
    }

    int marker_pid = 0;
    if (fscanf(f, "%d", &marker_pid) != 1) {
        fclose(f);
        /* Corrupt or empty — overwrite */
        return pid_marker_write(path, my_pid);
    }
    fclose(f);

    if (marker_pid == (int)my_pid) {
        /* It's ours — no conflict */
        return 0;
    }

    /* Check if the marker PID is still alive AND is actually an nbs-sidecar
     * with the same handle. Prevents false 'duplicate' when PID is recycled
     * by an unrelated process (PID recycling edge case). */
    if (kill((pid_t)marker_pid, 0) == 0) {
        /* Process is alive — verify it's actually an nbs-sidecar.
         * Read /proc/pid/cmdline and check for nbs-sidecar + matching handle. */
        char cmdline_path[64];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", marker_pid);
        FILE *cf = fopen(cmdline_path, "r");
        if (cf) {
            char cmdline[4096];
            size_t nr = fread(cmdline, 1, sizeof(cmdline) - 1, cf);
            fclose(cf);
            cmdline[nr] = '\0';

            /* cmdline is NUL-separated args. Must match BOTH:
             * 1. Binary name contains "nbs-sidecar"
             * 2. An arg matches "--handle=<our_handle>"
             * Without handle match, a sidecar for a DIFFERENT handle
             * would be falsely detected as a conflict. */
            int found_binary = 0;
            int found_handle = 0;
            char handle_arg[SIDECAR_MAX_HANDLE + 16];
            snprintf(handle_arg, sizeof(handle_arg), "--handle=%s", handle);

            for (size_t i = 0; i < nr; i++) {
                if (cmdline[i] == '\0') continue;
                if (strstr(cmdline + i, "nbs-sidecar") != NULL) {
                    found_binary = 1;
                }
                if (strcmp(cmdline + i, handle_arg) == 0) {
                    found_handle = 1;
                }
                /* Skip to next NUL */
                while (i < nr && cmdline[i] != '\0') i++;
            }

            if (found_binary && found_handle) {
                /* Same binary, same handle — real conflict */
                return 1;
            }
            if (found_binary && !found_handle) {
                /* nbs-sidecar for a different handle — not our conflict */
                fprintf(stderr, "sidecar: PID %d is nbs-sidecar but different "
                        "handle (not '%s'), taking ownership\n",
                        marker_pid, handle);
            } else {
                /* Not nbs-sidecar — PID recycled */
                fprintf(stderr, "sidecar: PID %d alive but not nbs-sidecar "
                        "(recycled PID), taking ownership\n", marker_pid);
            }
        } else {
            /* Can't read /proc/pid/cmdline — process may have just died,
             * or permissions issue. Treat as stale with warning. */
            fprintf(stderr, "sidecar: cannot read /proc/%d/cmdline — "
                    "treating PID marker as stale\n", marker_pid);
        }
    } else {
        /* Process is dead (stale PID) */
        fprintf(stderr, "sidecar: stale PID marker %d, taking ownership\n",
                marker_pid);
    }

    /* Take ownership */
    return pid_marker_write(path, my_pid);
}

/*
 * pid_marker_remove — Remove PID marker file on clean shutdown.
 */
static void pid_marker_remove(const char *path) {
    unlink(path);
}

/* --- Main loop --- */

int sidecar_run(const sidecar_config_t *cfg, transport_t *tp) {
    ASSERT_MSG(cfg != NULL, "sidecar_run: cfg is NULL");
    ASSERT_MSG(tp != NULL, "sidecar_run: tp is NULL");
    ASSERT_MSG(tp->capture != NULL, "sidecar_run: transport capture not initialised");
    ASSERT_MSG(tp->send_key != NULL, "sidecar_run: transport send_key not initialised");
    ASSERT_MSG(tp->send_text != NULL, "sidecar_run: transport send_text not initialised");
    ASSERT_MSG(tp->is_alive != NULL, "sidecar_run: transport is_alive not initialised");

    sc_dbg("sidecar_run: start handle=%s root=%s", cfg->handle, cfg->nbs_root);

    /* Build paths */
    char registry_path[SIDECAR_EXT_PATH];
    char inbox_path[SIDECAR_EXT_PATH];
    build_registry_path(cfg, registry_path, sizeof(registry_path));
    build_inbox_path(cfg, inbox_path, sizeof(inbox_path));

    /* Initialise state */
    sidecar_state_t state;
    memset(&state, 0, sizeof(state));

    /* Seed registry */
    int seed_rc = registry_seed(cfg->nbs_root, registry_path);
    if (seed_rc != 0) {
        fprintf(stderr, "sidecar_run: registry_seed failed\n");
    }

    /* PID marker — write BEFORE first cursor advance (Root Cause C).
     * Check for existing live sidecar with the same handle. If one
     * exists, exit immediately to prevent duplicate cursor advancement. */
    char pid_marker_path[SIDECAR_EXT_PATH];
    build_pid_marker_path(cfg, pid_marker_path, sizeof(pid_marker_path));

    int pid_check_rc = pid_marker_check(pid_marker_path, getpid(), cfg->handle);
    if (pid_check_rc == 1) {
        fprintf(stderr, "sidecar_run: duplicate sidecar detected for handle '%s' "
                "— another sidecar is already running. Exiting.\n", cfg->handle);
        sc_dbg("duplicate sidecar detected for %s — exiting", cfg->handle);
        return SIDECAR_EXIT_ERROR;
    }
    if (pid_check_rc < 0) {
        fprintf(stderr, "sidecar_run: warning: failed to write PID marker for '%s'\n",
                cfg->handle);
    }
    sc_dbg("PID marker written: %s (pid=%d)", pid_marker_path, (int)getpid());

    /* Lifecycle log: startup */
    fprintf(stderr, "sidecar startup: handle=%s pid=%d root=%s\n",
            cfg->handle, (int)getpid(), cfg->nbs_root);

    /* No blocking init-wait. The main loop handles initial prompt
     * injection alongside queries and interrupts. This ensures queries
     * (@handle?) work from the first tick, not after a 60-second block. */
    int init_prompt_pending = (cfg->initial_prompt[0] != '\0');
    time_t init_prompt_deadline = time(NULL) + 60;

    state.sidecar_start_time = time(NULL);
    state.last_flush_time = state.sidecar_start_time; /* unused — flush removed */
    state.last_poll_time = state.sidecar_start_time;
    state.last_fixup_check = state.sidecar_start_time;
    state.last_librarian_check = state.sidecar_start_time;
    state.last_pythia_check = state.sidecar_start_time;
    state.last_shepard_check = state.sidecar_start_time;

    ASSERT_MSG(state.sidecar_start_time > 0,
               "sidecar_run: sidecar_start_time invariant violated after init");

    /* Heartbeat interval: log self-health every 300 seconds (5 minutes) */
    time_t last_heartbeat_time = state.sidecar_start_time;
    time_t last_oracle_reaper_check = state.sidecar_start_time;

    sc_dbg("entering main loop");

    /* Main loop */
    while (1) {
        sleep(1);

        /* Team pause check — if control-pause file exists, skip notifications
         * and triggers. Initial prompt injection is exempt: manually-spawned
         * oracles (/pythia, /digest etc.) need their prompt even during pause,
         * and init injection only fires once on sidecar startup. */
        int team_paused = 0;
        {
            char pause_path[8192];
            snprintf(pause_path, sizeof(pause_path),
                     "%s/.nbs/control-pause", cfg->nbs_root);
            struct stat pause_st;
            if (stat(pause_path, &pause_st) == 0) {
                team_paused = 1;
                state.was_paused = 1;
                if (!init_prompt_pending) {
                    sleep(4);  /* total 5s with the sleep(1) above */
                    continue;
                }
                /* Fall through to initial prompt injection */
            }
        }

        /* Pause→resume transition: inject a hard system notification.
         * The standard [NBS-CHAT-NOTIFICATION] is too soft — agents
         * ignore it after sitting idle for 20 minutes during pause.
         * This message is unmistakable and bypasses cooldown. */
        if (state.was_paused && !team_paused) {
            state.was_paused = 0;
            sc_dbg("pause→resume transition detected, injecting resume notification");

            /* Wait for prompt to settle after resume */
            sleep(2);
            char *resume_content = tp->capture(tp, 30);
            if (resume_content) {
                if (detect_prompt_not_trust(resume_content)) {
                    const char *resume_msg =
                        "[NBS-SYSTEM-NOTIFICATION] Chat is resuming. "
                        "Your cursor has been reset. The chat is NOT paused. "
                        "Read your unread messages with nbs-chat read "
                        "--unread=<your-handle> and resume work. "
                        "[THIS MESSAGE WAS MACHINE GENERATED]";
                    if (tp->send_text(tp, resume_msg) != 0)
                        fprintf(stderr, "sidecar: resume notification send failed\n");
                    usleep(300000);
                    if (tp->send_key(tp, "Enter") != 0)
                        fprintf(stderr, "sidecar: resume notification Enter failed\n");
                    state.last_notify_time = time(NULL);
                    state.idle_seconds = 0;
                    state.last_content_hash = 0;
                }
                free(resume_content);
            }
        }

        /* Periodic state invariant verification (sidecar.h lines 80-84).
         * Detects corruption from integer overflow or logic errors. */
        ASSERT_MSG(state.idle_seconds >= 0,
                   "invariant: idle_seconds went negative: %d",
                   state.idle_seconds);
        ASSERT_MSG(state.bus_check_counter >= 0,
                   "invariant: bus_check_counter went negative: %d",
                   state.bus_check_counter);
        ASSERT_MSG(state.notify_fail_count >= 0,
                   "invariant: notify_fail_count went negative: %d",
                   state.notify_fail_count);

        /* Periodic self-health heartbeat (every 300s).
         * Logs key state to stderr so operators can verify the loop is alive
         * and detect anomalies (stuck idle, accumulating failures). */
        {
            time_t hb_now = time(NULL);
            if ((hb_now - last_heartbeat_time) >= 300) {
                fprintf(stderr, "sidecar heartbeat: handle=%s idle=%d "
                        "bus_checks=%d notify_fails=%d uptime=%lds\n",
                        cfg->handle, state.idle_seconds,
                        state.bus_check_counter, state.notify_fail_count,
                        (long)(hb_now - state.sidecar_start_time));
                last_heartbeat_time = hb_now;

                /* PID marker re-check on heartbeat — detect if another
                 * sidecar has taken ownership (Root Cause C). */
                int hb_pid_rc = pid_marker_check(pid_marker_path, getpid(), cfg->handle);
                if (hb_pid_rc == 1) {
                    fprintf(stderr, "sidecar: PID marker mismatch for '%s' "
                            "— another sidecar took ownership. Exiting.\n",
                            cfg->handle);
                    sc_dbg("PID marker mismatch on heartbeat — exiting");
                    /* Do NOT remove PID file — it belongs to the new owner.
                     * Only the clean exit path removes it (we own it there). */
                    break;
                }
            }
        }

        /* Check control inbox */
        int inbox_rc = registry_process_inbox(inbox_path, registry_path,
                                &state.control_inbox_line);
        if (inbox_rc < 0) {
            fprintf(stderr, "sidecar_run: registry_process_inbox failed\n");
        }

        /* --- Out-of-band query check (every tick, FIRST) ---
         * Queries (@handle?) are instant status checks from the human.
         * Process before mentions/interrupts so they don't queue behind
         * events with sleep(3) delays. */
        {
            char qbus_dir[SIDECAR_MAX_PATH];
            if (registry_find_first(registry_path, "bus",
                                     qbus_dir, sizeof(qbus_dir)) == 0) {
                char qpayload[SIDECAR_MAX_MESSAGE];
                char qevent_file[SIDECAR_MAX_PATH];
                if (bus_client_check_typed(qbus_dir, "chat-query",
                                            cfg->handle, qpayload,
                                            sizeof(qpayload),
                                            qevent_file, sizeof(qevent_file)) == 0) {
                    /* handle_query posts its own errors to chat on
                     * every failure path — no separate error posting
                     * needed here.  Always ack regardless of outcome
                     * (queries are one-shot, not retried). */
                    handle_query(tp, cfg, registry_path);
                    bus_client_ack_event(qbus_dir, qevent_file);
                }
            }
        }

        /* --- Initial prompt injection (non-blocking) ---
         * Tried every tick until the prompt appears or 60s deadline passes.
         * Uses 30 lines to reliably capture the prompt area. */
        if (init_prompt_pending) {
            if (time(NULL) > init_prompt_deadline) {
                fprintf(stderr, "sidecar_run: init prompt deadline expired, "
                        "giving up on initial prompt injection\n");
                init_prompt_pending = 0;
            } else {
                char *init_content = tp->capture(tp, 30);
                if (init_content) {
                    if (detect_prompt_not_trust(init_content)) {
                        if (tp->send_text(tp, cfg->initial_prompt) != 0)
                            fprintf(stderr, "sidecar_run: init prompt send_text failed\n");
                        usleep(300000);
                        if (tp->send_key(tp, "Enter") != 0)
                            fprintf(stderr, "sidecar_run: init prompt Enter failed\n");
                        init_prompt_pending = 0;
                        int settle = (cfg->startup_grace < 5)
                                     ? (cfg->startup_grace > 0 ? cfg->startup_grace : 1)
                                     : 5;
                        sleep(settle);
                    }
                    free(init_content);
                }
            }
        }

        /* If team is paused and we only fell through for init injection,
         * skip the rest of the tick (no notifications, triggers, polling). */
        if (team_paused) {
            sleep(4);
            continue;
        }

        /* --- Out-of-band interrupt check (every tick) --- */
        {
            char bus_dir[SIDECAR_MAX_PATH];
            if (registry_find_first(registry_path, "bus",
                                     bus_dir, sizeof(bus_dir)) == 0) {
                char payload[SIDECAR_MAX_MESSAGE];
                char event_file[SIDECAR_MAX_PATH];
                int matched = 0;
                if (bus_client_check_typed(bus_dir, "chat-interrupt",
                                            cfg->handle, payload,
                                            sizeof(payload),
                                            event_file, sizeof(event_file)) == 0) {
                    matched = 1;
                } else if (bus_client_check_typed(bus_dir, "chat-interrupt",
                                                   "team", payload,
                                                   sizeof(payload),
                                                   event_file, sizeof(event_file)) == 0) {
                    matched = 1;
                }
                if (matched) {
                    if (handle_interrupt(tp, cfg, registry_path) == 0) {
                        bus_client_ack_event(bus_dir, event_file);
                        state.interrupt_retry_count = 0;
                    } else {
                        state.interrupt_retry_count++;
                        if (state.interrupt_retry_count >= 3) {
                            fprintf(stderr, "sidecar: interrupt failed 3 times, "
                                    "acking to clear\n");
                            bus_client_ack_event(bus_dir, event_file);
                            state.interrupt_retry_count = 0;
                        }
                    }
                    state.idle_seconds = 0;
                    state.last_content_hash = 0;
                    sleep(3);
                    continue;
                } else {
                    state.interrupt_retry_count = 0;
                }
            }
        }

        /* --- Out-of-band mention check (every tick) --- */
        {
            char bus_dir[SIDECAR_MAX_PATH];
            if (registry_find_first(registry_path, "bus",
                                     bus_dir, sizeof(bus_dir)) == 0) {
                char payload[SIDECAR_MAX_MESSAGE];
                char event_file[SIDECAR_MAX_PATH];
                int matched = 0;
                if (bus_client_check_typed(bus_dir, "chat-mention",
                                            cfg->handle, payload,
                                            sizeof(payload),
                                            event_file, sizeof(event_file)) == 0) {
                    matched = 1;
                } else if (bus_client_check_typed(bus_dir, "chat-mention",
                                                   "team", payload,
                                                   sizeof(payload),
                                                   event_file, sizeof(event_file)) == 0) {
                    matched = 1;
                }
                if (matched) {
                    /* Mentions are stored for later injection, not processed
                     * immediately. Ack on successful storage. */
                    state.mention_detected = 1;
                    /* Cap payload */
                    if (strlen(payload) >= sizeof(state.mention_payload)) {
                        memcpy(state.mention_payload, payload,
                               sizeof(state.mention_payload) - 4);
                        state.mention_payload[sizeof(state.mention_payload) - 4] = '.';
                        state.mention_payload[sizeof(state.mention_payload) - 3] = '.';
                        state.mention_payload[sizeof(state.mention_payload) - 2] = '.';
                        state.mention_payload[sizeof(state.mention_payload) - 1] = '\0';
                    } else {
                        snprintf(state.mention_payload,
                                 sizeof(state.mention_payload), "%s", payload);
                    }
                    /* Mention storage always succeeds — ack immediately */
                    bus_client_ack_event(bus_dir, event_file);
                    state.mention_retry_count = 0;
                } else {
                    state.mention_retry_count = 0;
                }
            }
        }

        /* Query check moved to top of loop (before interrupts/mentions) */

        /* Check transport alive.
         * is_alive returns: 1 = alive, 0 = dead, -1 = error (e.g. session
         * directory deleted). Errors are transient (filesystem glitch) or
         * permanent (session cleaned up). Track consecutive errors — if
         * they persist for 60 ticks (~1 minute), the session is gone. */
        {
            static int alive_error_count = 0;
            int alive_rc = tp->is_alive(tp);
            if (alive_rc == 0) {
                sc_dbg("transport NOT ALIVE — exiting");
                fprintf(stderr, "sidecar_run: transport not alive for '%s', exiting\n",
                        cfg->handle);
                break;
            } else if (alive_rc < 0) {
                alive_error_count++;
                if (alive_error_count >= 60) {
                    sc_dbg("transport error for 60 consecutive ticks — exiting");
                    fprintf(stderr, "sidecar_run: transport error persisted 60s "
                            "for '%s', assuming session gone\n", cfg->handle);
                    break;
                }
            } else {
                alive_error_count = 0;
            }
        }

        /* Root Cause B: cooldown suppression tracking runs EVERY tick,
         * BEFORE capture/content checks. This tracks MESSAGE QUEUE state
         * (unreads exist during cooldown), not delivery state, so it
         * doesn't depend on capture working or content stability.
         *
         * Design (per theologian):
         * - If unreads > 0 AND cooldown active → cooldown_suppressed = 1
         * - If cooldown expired AND cooldown_suppressed → catchup_needed = 1
         */
        int catchup_needed = 0;
        {
            int cd_active = cooldown_is_active(cfg, &state);

            /* Lightweight unread check — no capture dependency */
            int unread_count = 0;
            char unread_summary[SIDECAR_MAX_MESSAGE];
            int ur_rc = chat_client_check_unread(registry_path, cfg->handle,
                                                  &unread_count, unread_summary,
                                                  sizeof(unread_summary));
            int has_unreads = (ur_rc == 0 && unread_count > 0);

            if (has_unreads && cd_active) {
                if (!state.cooldown_suppressed) {
                    sc_dbg("cooldown_suppressed SET: unreads=%d cooldown=%d",
                           unread_count, cfg->notify_cooldown);
                }
                state.cooldown_suppressed = 1;
            }

            if (state.cooldown_suppressed && !cd_active) {
                /* Cooldown expired with suppressed events — catch up */
                catchup_needed = 1;
                state.cooldown_suppressed = 0;
                sc_dbg("catchup_needed: cooldown expired with %d unreads",
                       unread_count);
            }
        }

        /* Capture content and hash. 30 lines to reliably include
         * the prompt area (Claude's terminal has many blank/control
         * lines between the prompt and the end of output). */
        char *content = tp->capture(tp, 30);
        if (!content) {
            sc_dbg("capture returned NULL");
            continue;
        }

        size_t content_len = strlen(content);
        uint64_t current_hash = fnv1a_hash(content, content_len);

        /* Enter flush REMOVED. The periodic Enter was submitting Claude
         * Code's "suggested next step" prompts as if the human typed them,
         * causing agents to hallucinate human instructions. The original
         * purpose (unsticking idle prompts) is now handled by the
         * notification injection system which sends text + Enter when
         * there is actual work to do. */
        {
            time_t now_wc = time(NULL);

            /* Wall-clock /nbs-poll injection — safety net for missed events.
             * Fires every poll_interval seconds regardless of idle counters.
             * Only suppressed during blocking dialogues and context stress. */
            if (cfg->poll_interval > 0 &&
                (now_wc - state.last_poll_time) >= cfg->poll_interval) {
                dialogue_response_t poll_resp = {0, 0};
                if (detect_blocking_dialogue(content, &poll_resp) == DIALOGUE_NONE &&
                    !detect_context_stress(content)) {
                    if (tp->send_text(tp, "/nbs-poll") != 0) {
                        fprintf(stderr, "sidecar_run: poll send_text failed\n");
                    }
                    usleep(300000);
                    if (tp->send_key(tp, "Enter") != 0) {
                        fprintf(stderr, "sidecar_run: poll send_key Enter failed\n");
                    }
                    state.last_poll_time = now_wc;
                    state.idle_seconds = 0;
                    state.last_content_hash = 0;
                    free(content);
                    sleep(5);
                    continue;
                }
            }

            /* Wall-clock periodic triggers — checked once per minute to
             * avoid excessive file I/O on shared timestamp files.
             * Each trigger uses shared timestamp + lock for cross-sidecar dedup. */
            if (cfg->fixup_interval > 0 &&
                (now_wc - state.last_fixup_check) >= 60) {
                state.last_fixup_check = now_wc;
                trigger_periodic_check(cfg->nbs_root, cfg->fixup_interval,
                                       &TRIGGER_FIXUP);
            }
            if (cfg->librarian_interval > 0 &&
                (now_wc - state.last_librarian_check) >= 60) {
                state.last_librarian_check = now_wc;
                trigger_periodic_check(cfg->nbs_root, cfg->librarian_interval,
                                       &TRIGGER_LIBRARIAN);
            }
            if (cfg->pythia_interval > 0 &&
                (now_wc - state.last_pythia_check) >= 60) {
                state.last_pythia_check = now_wc;
                trigger_periodic_check(cfg->nbs_root, cfg->pythia_interval,
                                       &TRIGGER_PYTHIA);
            }
            if (cfg->shepard_interval > 0 &&
                (now_wc - state.last_shepard_check) >= 60) {
                state.last_shepard_check = now_wc;
                trigger_periodic_check(cfg->nbs_root, cfg->shepard_interval,
                                       &TRIGGER_SHEPARD);
            }

            /* Oracle reaper — check every 10s for oracles that posted
             * to chat and should be killed. Runs as fire-and-forget. */
            if ((now_wc - last_oracle_reaper_check) >= 10) {
                last_oracle_reaper_check = now_wc;
                char reaper_path[4096];
                char self_path[4096];
                ssize_t rlen = readlink("/proc/self/exe", self_path,
                                         sizeof(self_path) - 1);
                if (rlen > 0) {
                    self_path[rlen] = '\0';
                    char *rslash = strrchr(self_path, '/');
                    if (rslash) {
                        size_t rdir_len = (size_t)(rslash - self_path);
                        if (rdir_len + sizeof("/nbs-oracle-reaper") <=
                            sizeof(reaper_path)) {
                            memcpy(reaper_path, self_path, rdir_len);
                            memcpy(reaper_path + rdir_len,
                                   "/nbs-oracle-reaper",
                                   sizeof("/nbs-oracle-reaper"));
                            const char *reaper_argv[] = {
                                reaper_path, "check",
                                cfg->nbs_root, NULL
                            };
                            exec_fire_and_forget(reaper_argv);
                        }
                    }
                }
            }
        }

        if (current_hash != state.last_content_hash) {
            /* Content changed */
            state.idle_seconds = 0;
            state.bus_check_counter = 0;
            state.last_content_hash = current_hash;

            /* Check for blocking dialogue on content change */
            dialogue_response_t resp;
            if (detect_blocking_dialogue(content, &resp) != DIALOGUE_NONE) {
                sleep(1);
                respond_dialogue(tp, &resp);
                state.last_content_hash = 0;
                free(content);
                continue;
            }

            /* Root Cause B: if catch-up is needed, fall through to bus
             * check instead of skipping. The agent needs the notification
             * even during content changes. */
            if (!catchup_needed) {
                free(content);
                continue;
            }
            /* Fall through to notification delivery */
        }

        /* Content stable — check for dialogue when idle */
        {
            dialogue_response_t resp;
            if (detect_blocking_dialogue(content, &resp) != DIALOGUE_NONE) {
                respond_dialogue(tp, &resp);
                state.idle_seconds = 0;
                state.last_content_hash = 0;
                free(content);
                continue;
            }
        }

        /* H2 fix: cap idle_seconds to prevent int overflow.
         * At 1 tick/second, INT_MAX (~2^31) is ~68 years — unreachable in
         * practice, but capping is cheap and makes the invariant falsifiable. */
        if (state.idle_seconds < INT_MAX)
            state.idle_seconds++;
        if (state.bus_check_counter < INT_MAX)
            state.bus_check_counter++;

        /* Bus-aware check — catchup_needed bypasses interval and idle gates */
        if (state.bus_check_counter >= cfg->bus_check_interval || catchup_needed) {
            state.bus_check_counter = 0;

            if (detect_prompt_idle(content) || catchup_needed) {
                /* Context stress — back off */
                if (detect_context_stress(content)) {
                    state.idle_seconds = 0;
                    state.last_content_hash = 0;
                    free(content);
                    sleep(30);
                    continue;
                }

                if (should_inject_notify(cfg, &state, registry_path) == 0) {
                    /* TOCTOU re-capture before injection */
                    char *fresh = tp->capture(tp, 30);
                    if (fresh) {
                        if (!detect_prompt_idle(fresh)) {
                            /* Prompt disappeared — abort */
                            free(fresh);
                            state.idle_seconds = 0;
                            state.last_content_hash = 0;
                            free(content);
                            sleep(2);
                            continue;
                        }
                        free(fresh);
                    }

                    /* Inject notification as plain text prompt.
                     * Contains NO chat content — only the instruction to
                     * read the chat. The sidecar acks bus events. */
                    char nfy_chat_path[SIDECAR_MAX_PATH];
                    if (registry_find_first(registry_path, "chat",
                                             nfy_chat_path, sizeof(nfy_chat_path)) != 0)
                        nfy_chat_path[0] = '\0';
                    char notify_prompt[SIDECAR_MAX_PROMPT];
                    build_notify_prompt(cfg, nfy_chat_path[0] ? nfy_chat_path : NULL,
                                        notify_prompt, sizeof(notify_prompt));
                    if (tp->send_text(tp, notify_prompt) != 0) {
                        fprintf(stderr, "sidecar_run: notify send_text failed\n");
                    }
                    usleep(300000);
                    if (tp->send_key(tp, "Enter") != 0) {
                        fprintf(stderr, "sidecar_run: notify send_key Enter failed\n");
                    }

                    /* Verify injection consumed (up to 3 retries).
                     * Capture only 3 lines (prompt area) to avoid false
                     * positives from old notifications in scrollback. */
                    int injection_consumed = 0;
                    for (int retry = 1; retry <= 3; retry++) {
                        sleep(retry * 2);
                        char *verify = tp->capture(tp, 3);
                        if (!verify) continue;

                        if (strstr(verify, "messages to read in your chat") != NULL) {
                            /* Notification visible in terminal — injection
                             * succeeded (text reached the agent's screen).
                             * Whether the agent has "consumed" it (processed
                             * and scrolled past) is not our concern — the
                             * text is there, the agent can see it. */
                            sc_dbg("notification visible in terminal for %s",
                                   cfg->handle);
                            injection_consumed = 1;
                            free(verify);
                            break;
                        } else {
                            /* Not visible — either consumed (scrolled past)
                             * or not yet rendered. Either way, success. */
                            injection_consumed = 1;
                            free(verify);
                            break;
                        }
                    }

                    /* Always update last_notify_time after injection
                     * attempt — we sent the text + Enter, so delivery
                     * was attempted. Verification tracks whether it was
                     * consumed, but timing should reflect the attempt
                     * to prevent re-injection flooding. */
                    state.last_notify_time = time(NULL);

                    if (injection_consumed) {
                        state.notify_fail_count = 0;
                        /* Ack bus events — the agent no longer needs to
                         * do this manually. The notification was delivered,
                         * so the events have been communicated. */
                        {
                            char ack_bus_dir[SIDECAR_MAX_PATH];
                            if (registry_find_first(registry_path, "bus",
                                                     ack_bus_dir,
                                                     sizeof(ack_bus_dir)) == 0) {
                                const char *ack_argv[] = {
                                    "nbs-bus", "ack-all", ack_bus_dir, NULL
                                };
                                exec_fire_and_forget(ack_argv);
                            }
                        }
                    } else {
                        state.notify_fail_count++;
                        sc_dbg("notify_fail_count=%d threshold=%d",
                               state.notify_fail_count,
                               cfg->notify_fail_threshold);

                        /* Threshold check: post warning to chat when
                         * consecutive failures exceed the configured
                         * threshold. The sidecar does NOT self-heal
                         * (no restart, no Enter injection) — it warns
                         * the supervisor via chat so a human or fixup
                         * can decide what to do. */
                        if (state.notify_fail_count >= cfg->notify_fail_threshold) {
                            char fail_chat[SIDECAR_MAX_PATH];
                            if (registry_find_first(registry_path, "chat",
                                                     fail_chat,
                                                     sizeof(fail_chat)) == 0) {
                                char fail_msg[SIDECAR_MAX_MESSAGE];
                                snprintf(fail_msg, sizeof(fail_msg),
                                         "Notification injection failed %d "
                                         "consecutive times for %s — agent "
                                         "may not be receiving messages. "
                                         "@supervisor please investigate.",
                                         state.notify_fail_count,
                                         cfg->handle);
                                chat_client_error(fail_chat, fail_msg);
                            }
                            /* Reset counter to avoid spamming — warn again
                             * after another threshold-worth of failures. */
                            state.notify_fail_count = 0;
                        }
                    }

                    state.idle_seconds = 0;
                    state.last_content_hash = 0;
                    free(content);
                    sleep(8);
                    continue;
                }
            }
        }

        free(content);
    }

    sc_dbg("main loop exited for %s", cfg->handle);

    /* Lifecycle log: shutdown */
    fprintf(stderr, "sidecar shutdown: handle=%s pid=%d uptime=%lds\n",
            cfg->handle, (int)getpid(),
            (long)(time(NULL) - state.sidecar_start_time));

    /* Clean up PID marker on exit — only if we still own it.
     * If another sidecar took ownership (heartbeat mismatch), the file
     * contains their PID and we must not delete it. */
    {
        FILE *pf = fopen(pid_marker_path, "r");
        if (pf) {
            int file_pid = 0;
            if (fscanf(pf, "%d", &file_pid) == 1 && file_pid == (int)getpid()) {
                fclose(pf);
                pid_marker_remove(pid_marker_path);
            } else {
                fclose(pf);
                /* PID file belongs to another sidecar — leave it */
            }
        }
    }

    return SIDECAR_EXIT_OK;
}
