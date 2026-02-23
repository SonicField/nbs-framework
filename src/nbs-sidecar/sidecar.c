/*
 * sidecar.c — Main sidecar loop and notification engine.
 *
 * Ported from the bash implementation in bin/nbs-claude.
 * The loop runs at a 1-second tick, checking for:
 *   - Interrupt events (every tick, unconditional)
 *   - Mention events (every tick, sets flag)
 *   - Blocking dialogues (on content change and when idle)
 *   - Bus events and chat unreads (every BUS_CHECK_INTERVAL ticks)
 *   - Periodic triggers (pythia, standup, heartbeat)
 *   - Periodic Enter flush (every FLUSH_INTERVAL ticks)
 *
 * The transport vtable abstracts tmux vs pty-session interactions.
 */

#include "sidecar.h"
#include "transport.h"
#include "detect.h"
#include "hash.h"
#include "bus_client.h"
#include "chat_client.h"
#include "registry.h"
#include "triggers.h"
#include "exec_util.h"
#include "strip_ansi.h"
#include "mention_escape.h"
#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Path buffer with room for prefix + handle suffix */
#define SIDECAR_EXT_PATH (SIDECAR_MAX_PATH + SIDECAR_MAX_HANDLE + 32)

/* --- Path builders --- */

static void build_registry_path(const sidecar_config_t *cfg,
                                 char *out, size_t out_size) {
    ASSERT_MSG(cfg != NULL, "build_registry_path: cfg is NULL");
    ASSERT_MSG(out != NULL, "build_registry_path: out is NULL");
    ASSERT_MSG(out_size > 0, "build_registry_path: out_size is 0");

    snprintf(out, out_size, "%s/.nbs/control-registry-%s",
             cfg->nbs_root, cfg->handle);
}

static void build_inbox_path(const sidecar_config_t *cfg,
                              char *out, size_t out_size) {
    ASSERT_MSG(cfg != NULL, "build_inbox_path: cfg is NULL");
    ASSERT_MSG(out != NULL, "build_inbox_path: out is NULL");
    ASSERT_MSG(out_size > 0, "build_inbox_path: out_size is 0");

    snprintf(out, out_size, "%s/.nbs/control-inbox-%s",
             cfg->nbs_root, cfg->handle);
}

/* --- Recovery prompt builder --- */

/*
 * build_recovery_prompt — Construct a raw text prompt to re-bootstrap
 * the agent when /nbs-notify skill is lost after compaction.
 */
static void build_recovery_prompt(const sidecar_config_t *cfg,
                                   const char *registry_path,
                                   char *out, size_t out_size) {
    ASSERT_MSG(cfg != NULL, "build_recovery_prompt: cfg is NULL");
    ASSERT_MSG(registry_path != NULL, "build_recovery_prompt: registry_path is NULL");
    ASSERT_MSG(out != NULL, "build_recovery_prompt: out is NULL");
    ASSERT_MSG(out_size > 0, "build_recovery_prompt: out_size is 0");

    char chat_path[SIDECAR_MAX_PATH];
    int has_chat = (registry_find_first(registry_path, "chat",
                                         chat_path, sizeof(chat_path)) == 0);

    /* Build in pieces to avoid format-truncation warnings */
    size_t off = 0;
    off += (size_t)snprintf(out + off, out_size - off,
        "Your skills were lost after compaction. Please read these files "
        "to restore them: ");
    if (off < out_size)
        off += (size_t)snprintf(out + off, out_size - off,
            "%s/claude_tools/nbs-notify.md, ", cfg->nbs_root);
    if (off < out_size)
        off += (size_t)snprintf(out + off, out_size - off,
            "%s/claude_tools/nbs-teams-chat.md, ", cfg->nbs_root);
    if (off < out_size)
        off += (size_t)snprintf(out + off, out_size - off,
            "%s/claude_tools/nbs-poll.md. ", cfg->nbs_root);
    if (off < out_size)
        off += (size_t)snprintf(out + off, out_size - off,
            "Your handle is '%s'.", cfg->handle);
    if (has_chat && off < out_size) {
        /* Truncation is intentional — chat_path may be long */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        off += (size_t)snprintf(out + off, out_size - off,
            " Then send a message to %s confirming skills restored.",
            chat_path);
#pragma GCC diagnostic pop
    }
    (void)off;
}

/* --- Interrupt handler --- */

/*
 * handle_interrupt — Send Escape every 10s for 60s, inject /nbs-notify
 * on success, post URGENT on failure.
 */
static void handle_interrupt(transport_t *tp, const sidecar_config_t *cfg,
                              const char *registry_path) {
    time_t start = time(NULL);
    int succeeded = 0;

    while (1) {
        time_t elapsed = time(NULL) - start;
        if (elapsed >= 60) break;

        /* Send Escape */
        tp->send_key(tp, "Escape");

        /* Wait up to 10s, checking for prompt every 1s */
        for (int w = 0; w < 10; w++) {
            sleep(1);

            char *content = tp->capture(tp, 5);
            if (!content) continue;

            if (detect_prompt_visible(content)) {
                /* Inject /nbs-notify interrupt */
                tp->send_text(tp, "/nbs-notify interrupt from chat");
                usleep(300000);
                tp->send_key(tp, "Enter");
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
            snprintf(msg, sizeof(msg),
                     "URGENT: @team - agent unresponsive %s", cfg->handle);
            chat_client_send(chat_path, "sidecar", msg);
        }
    }
}

/*
 * handle_query — Capture own pane content and post to chat.
 *
 * Triggered by @handle? in chat. The target agent's sidecar captures
 * its own pane, strips ANSI escapes, and posts the content to chat.
 */
static void handle_query(transport_t *tp, const sidecar_config_t *cfg,
                           const char *registry_path) {
    /* Capture own pane (100 lines of scrollback) */
    char *content = tp->capture(tp, 100);
    if (!content) return;

    /* Strip ANSI escape sequences */
    strip_ansi(content);

    /* Escape @ signs to prevent mention feedback loops.
     * The pane content may contain @handle references which would
     * trigger re-extraction by bus_extract_mentions.
     * Layer 1: sanitise_at_signs replaces all @ with \xc0 (primary defence).
     * Layer 2: escape_mentions inserts \ after @ (independently testable fallback). */
    sanitise_at_signs(content);
    char *escaped = escape_mentions(content);

    /* Find first registered chat and send */
    char chat_path[SIDECAR_MAX_PATH];
    if (registry_find_first(registry_path, "chat",
                             chat_path, sizeof(chat_path)) == 0) {
        char msg[SIDECAR_MAX_CONTENT];
        snprintf(msg, sizeof(msg),
                 "tmux pane for %s:\n%s", cfg->handle, escaped);
        chat_client_send(chat_path, "sidecar", msg);
    }

    free(escaped);
    free(content);
}

/* --- Dialogue response --- */

static void respond_dialogue(transport_t *tp,
                              const dialogue_response_t *resp) {
    ASSERT_MSG(tp != NULL, "respond_dialogue: tp is NULL");
    ASSERT_MSG(resp != NULL, "respond_dialogue: resp is NULL");
    ASSERT_MSG(resp->option > 0 && resp->option <= 9, "respond_dialogue: option must be in range [1, 9]");

    char option_str[4];
    snprintf(option_str, sizeof(option_str), "%d", resp->option);
    tp->send_text(tp, option_str);
    usleep(500000);
    tp->send_key(tp, "Enter");
    sleep(resp->settle_secs);
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

    /* Pythia trigger (side-effect: may publish event or spawn worker) */
    if (has_bus) {
        trigger_pythia_check(registry_path, cfg->nbs_root,
                              &state->pythia_last_trigger_count);
    }

    /* Shepard trigger (side-effect: may spawn worker) */
    if (has_bus) {
        trigger_shepard_check(registry_path, cfg->nbs_root,
                               &state->shepard_last_trigger_count);
    }

    /* Standup trigger (side-effect: may post to chat) */
    trigger_standup_check(registry_path, cfg->nbs_root, cfg->handle,
                           cfg->standup_interval,
                           &state->last_standup_time);

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

    /* Apply cooldown (critical and mentions bypass) */
    now = time(NULL);
    int elapsed = (int)(now - state->last_notify_time);

    if (strcmp(state->bus_max_priority, "critical") != 0 &&
        state->mention_detected != 1 &&
        elapsed < cfg->notify_cooldown) {
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

        if (state->bus_event_summary[0] != '\0') {
            off += (size_t)snprintf(parts + off, sizeof(parts) - off,
                                     "%s", state->bus_event_summary);
        }
        if (state->chat_unread_summary[0] != '\0') {
            if (off > 0) {
                off += (size_t)snprintf(parts + off, sizeof(parts) - off,
                                         ". %s", state->chat_unread_summary);
            } else {
                off += (size_t)snprintf(parts + off, sizeof(parts) - off,
                                         "%s", state->chat_unread_summary);
            }
        }
        (void)off; /* suppress unused warning */

        /* Cap at message limit */
        if (strlen(parts) > SIDECAR_MAX_MESSAGE - 1) {
            parts[SIDECAR_MAX_MESSAGE - 4] = '.';
            parts[SIDECAR_MAX_MESSAGE - 3] = '.';
            parts[SIDECAR_MAX_MESSAGE - 2] = '.';
            parts[SIDECAR_MAX_MESSAGE - 1] = '\0';
        }

        snprintf(state->notify_message, sizeof(state->notify_message),
                 "%s", parts);
    }

    ASSERT_MSG(state->notify_message[0] != '\0',
               "should_inject_notify: returning inject but message is empty");

    state->last_notify_time = now;
    return 0;
}

/* --- Config validation --- */

int sidecar_config_validate(const sidecar_config_t *cfg) {
    ASSERT_MSG(cfg != NULL, "sidecar_config_validate: cfg is NULL");

    int ok = 1;

    if (cfg->handle[0] == '\0') {
        fprintf(stderr, "config error: handle is empty\n");
        ok = 0;
    }
    if (cfg->nbs_root[0] == '\0') {
        fprintf(stderr, "config error: nbs_root is empty\n");
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

    return ok ? 0 : -1;
}

/* --- Main loop --- */

int sidecar_run(const sidecar_config_t *cfg, transport_t *tp) {
    ASSERT_MSG(cfg != NULL, "sidecar_run: cfg is NULL");
    ASSERT_MSG(tp != NULL, "sidecar_run: tp is NULL");
    ASSERT_MSG(tp->capture != NULL, "sidecar_run: transport not initialised");

    /* Build paths */
    char registry_path[SIDECAR_EXT_PATH];
    char inbox_path[SIDECAR_EXT_PATH];
    build_registry_path(cfg, registry_path, sizeof(registry_path));
    build_inbox_path(cfg, inbox_path, sizeof(inbox_path));

    /* Initialise state */
    sidecar_state_t state;
    memset(&state, 0, sizeof(state));

    /* Seed registry */
    registry_seed(cfg->nbs_root, registry_path);

    /* Wait for prompt and inject initial prompt */
    int init_wait = 0;
    while (init_wait < 60) {
        sleep(2);
        init_wait += 2;

        char *content = tp->capture(tp, 5);
        if (!content) continue;

        if (detect_prompt_visible(content)) {
            tp->send_text(tp, cfg->initial_prompt);
            usleep(300000);
            tp->send_key(tp, "Enter");
            /* Wait for initial prompt to be consumed.
             * Cap at startup_grace — with grace=0 we want fast start. */
            int init_settle = (cfg->startup_grace < 5)
                              ? (cfg->startup_grace > 0 ? cfg->startup_grace : 1)
                              : 5;
            sleep(init_settle);
            state.sidecar_start_time = time(NULL);
            state.last_flush_time = state.sidecar_start_time;
            state.last_poll_time = state.sidecar_start_time;
            free(content);
            break;
        }
        free(content);
    }

    /* Main loop */
    while (1) {
        sleep(1);

        /* Check control inbox */
        registry_process_inbox(inbox_path, registry_path,
                                &state.control_inbox_line);

        /* --- Out-of-band interrupt check (every tick) --- */
        {
            char bus_dir[SIDECAR_MAX_PATH];
            if (registry_find_first(registry_path, "bus",
                                     bus_dir, sizeof(bus_dir)) == 0) {
                char payload[SIDECAR_MAX_MESSAGE];
                if (bus_client_check_typed(bus_dir, "chat-interrupt",
                                            cfg->handle, payload,
                                            sizeof(payload)) == 0) {
                    handle_interrupt(tp, cfg, registry_path);
                    state.idle_seconds = 0;
                    state.last_content_hash = 0;
                    sleep(3);
                    continue;
                }
            }
        }

        /* --- Out-of-band mention check (every tick) --- */
        {
            char bus_dir[SIDECAR_MAX_PATH];
            if (registry_find_first(registry_path, "bus",
                                     bus_dir, sizeof(bus_dir)) == 0) {
                char payload[SIDECAR_MAX_MESSAGE];
                if (bus_client_check_typed(bus_dir, "chat-mention",
                                            cfg->handle, payload,
                                            sizeof(payload)) == 0) {
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
                }
            }
        }

        /* --- Out-of-band query check (every tick) --- */
        {
            char bus_dir[SIDECAR_MAX_PATH];
            if (registry_find_first(registry_path, "bus",
                                     bus_dir, sizeof(bus_dir)) == 0) {
                char payload[SIDECAR_MAX_MESSAGE];
                if (bus_client_check_typed(bus_dir, "chat-query",
                                            cfg->handle, payload,
                                            sizeof(payload)) == 0) {
                    handle_query(tp, cfg, registry_path);
                }
            }
        }

        /* Check transport alive */
        if (!tp->is_alive(tp)) {
            break;
        }

        /* Capture content and hash */
        char *content = tp->capture(tp, 5);
        if (!content) continue;

        size_t content_len = strlen(content);
        uint64_t current_hash = fnv1a_hash(content, content_len);

        /* Wall-clock Enter flush — fires regardless of idle state.
         * Ensures the UI is flushed periodically even when the content
         * hash is changing (agent appears busy). Only suppressed during
         * blocking dialogues where Enter would select an option. */
        {
            time_t now_wc = time(NULL);
            if (cfg->flush_interval > 0 &&
                (now_wc - state.last_flush_time) >= cfg->flush_interval) {
                if (detect_blocking_dialogue(content, NULL) == DIALOGUE_NONE) {
                    tp->send_key(tp, "Enter");
                    state.last_flush_time = now_wc;
                }
            }

            /* Wall-clock /nbs-poll injection — safety net for missed events.
             * Fires every poll_interval seconds regardless of idle counters. */
            if (cfg->poll_interval > 0 &&
                (now_wc - state.last_poll_time) >= cfg->poll_interval) {
                if (detect_prompt_visible(content) &&
                    !detect_context_stress(content)) {
                    tp->send_text(tp, "/nbs-poll");
                    usleep(300000);
                    tp->send_key(tp, "Enter");
                    state.last_poll_time = now_wc;
                    state.idle_seconds = 0;
                    state.last_content_hash = 0;
                    free(content);
                    sleep(5);
                    continue;
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

            /* Heartbeat */
            if (cfg->active_heartbeat > 0) {
                trigger_heartbeat(registry_path, cfg->handle,
                                   cfg->active_heartbeat,
                                   &state.last_heartbeat_time);
            }

            free(content);
            continue;
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

        state.idle_seconds++;
        state.bus_check_counter++;

        /* Bus-aware check */
        if (state.bus_check_counter >= cfg->bus_check_interval) {
            state.bus_check_counter = 0;

            if (detect_prompt_visible(content)) {
                /* Context stress — back off */
                if (detect_context_stress(content)) {
                    state.idle_seconds = 0;
                    state.last_content_hash = 0;
                    free(content);
                    sleep(30);
                    continue;
                }

                if (should_inject_notify(cfg, &state, registry_path) == 0) {
                    /* Self-heal check */
                    if (state.notify_fail_count >= cfg->notify_fail_threshold) {
                        char recovery[SIDECAR_MAX_PROMPT];
                        build_recovery_prompt(cfg, registry_path,
                                               recovery, sizeof(recovery));
                        tp->send_text(tp, recovery);
                        usleep(300000);
                        tp->send_key(tp, "Enter");

                        sleep(5);
                        char *rc_content = tp->capture(tp, 5);
                        if (rc_content) {
                            if (!detect_skill_failure(rc_content)) {
                                state.notify_fail_count = 0;
                            }
                            free(rc_content);
                        }

                        state.idle_seconds = 0;
                        state.last_content_hash = 0;
                        free(content);
                        sleep(15);
                        continue;
                    }

                    /* TOCTOU re-capture before injection */
                    char *fresh = tp->capture(tp, 5);
                    if (fresh) {
                        if (!detect_prompt_visible(fresh)) {
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

                    /* Inject /nbs-notify */
                    char inject_cmd[SIDECAR_MAX_MESSAGE + 16];
                    snprintf(inject_cmd, sizeof(inject_cmd),
                             "/nbs-notify %s", state.notify_message);
                    tp->send_text(tp, inject_cmd);
                    usleep(300000);
                    tp->send_key(tp, "Enter");

                    /* Verify injection consumed (up to 3 retries) */
                    int injection_consumed = 0;
                    for (int retry = 1; retry <= 3; retry++) {
                        sleep(retry * 2);
                        char *verify = tp->capture(tp, 5);
                        if (!verify) continue;

                        if (strstr(verify, "/nbs-notify") != NULL) {
                            /* Still in buffer — retry Enter */
                            tp->send_key(tp, "Enter");
                            free(verify);
                        } else {
                            /* Consumed */
                            injection_consumed = 1;
                            free(verify);
                            break;
                        }
                    }

                    if (injection_consumed) {
                        state.notify_fail_count = 0;
                    } else {
                        /* Check for skill failure */
                        char *final = tp->capture(tp, 5);
                        if (final) {
                            if (detect_prompt_visible(final) &&
                                detect_skill_failure(final)) {
                                state.notify_fail_count++;
                            }
                            free(final);
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

    return SIDECAR_EXIT_OK;
}
