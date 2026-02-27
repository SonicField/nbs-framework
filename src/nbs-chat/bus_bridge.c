/*
 * bus_bridge.c — Chat-to-bus event bridge implementation
 *
 * Publishes bus events via the nbs-bus binary when .nbs/events/ exists.
 * All bus failures are non-fatal — chat_send must never fail due to bus issues.
 *
 * @team expansion: when a message contains @team, @team!, or @team?,
 * the bridge reads the participants header from the chat file and publishes
 * individual events for each participant (excluding the sender). This ensures
 * every agent's sidecar receives its own event that it can ack independently,
 * avoiding the single-consumer bug where only the first sidecar to tick
 * would consume the shared @team event.
 */

#include "bus_bridge.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Maximum payload length passed to nbs-bus publish.
 * Messages longer than this are truncated in the event payload.
 * The full message is still in the chat file — the event is just a signal.
 */
#define MAX_PAYLOAD_LEN 2048

/*
 * Cached absolute path to the nbs-bus binary.
 * Resolved once on first use via /proc/self/exe — the binary is expected
 * to be in the same directory as nbs-chat (sibling binary).
 * If resolution fails, falls back to "nbs-bus" (PATH search via execlp).
 */
static char nbs_bus_path[MAX_PATH_LEN] = "";
static int nbs_bus_path_resolved = 0;

static const char *resolve_nbs_bus(void)
{
    if (nbs_bus_path_resolved)
        return nbs_bus_path[0] ? nbs_bus_path : "nbs-bus";

    nbs_bus_path_resolved = 1;

    char self[MAX_PATH_LEN];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len <= 0) {
        fprintf(stderr, "warning: resolve_nbs_bus: readlink /proc/self/exe failed\n");
        return "nbs-bus";
    }
    self[len] = '\0';

    /* Find last '/' to get directory */
    char *slash = strrchr(self, '/');
    if (!slash) {
        fprintf(stderr, "warning: resolve_nbs_bus: no '/' in /proc/self/exe path\n");
        return "nbs-bus";
    }

    /* Replace binary name with "nbs-bus" */
    size_t dir_len = (size_t)(slash - self);
    if (dir_len + sizeof("/nbs-bus") > sizeof(nbs_bus_path)) {
        fprintf(stderr, "warning: resolve_nbs_bus: path too long for nbs-bus sibling\n");
        return "nbs-bus";
    }

    memcpy(nbs_bus_path, self, dir_len);
    memcpy(nbs_bus_path + dir_len, "/nbs-bus", sizeof("/nbs-bus"));

    /* Verify it exists */
    if (access(nbs_bus_path, X_OK) != 0) {
        nbs_bus_path[0] = '\0';
        return "nbs-bus";
    }

    return nbs_bus_path;
}

/*
 * Maximum number of parent directories to walk when searching for
 * .nbs/events/. 10 is generous: project roots are typically 2-3 levels
 * above .nbs/chat/. The limit prevents unbounded traversal to /.
 */
#define MAX_DIR_WALK_DEPTH 10

/*
 * is_handle_char — Returns true if c is valid in a @handle.
 *
 * Handles can contain: a-z, A-Z, 0-9, underscore, hyphen.
 */
static int is_handle_char(int c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

/*
 * is_email_prefix_char — Returns true if c can precede @ in an email address.
 *
 * Email local parts can contain: a-z, A-Z, 0-9, dot, underscore, hyphen, plus.
 * If the character before @ is one of these, it's likely an email, not a mention.
 */
static int is_email_prefix_char(int c) {
    return isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '+';
}

/*
 * read_chat_participants — Extract participant handles from a chat file header.
 *
 * Reads only the header (up to the "---" separator). Parses the participants
 * line which has format: "participants: handle1(N1), handle2(N2), ..."
 *
 * Returns the number of participants found, or 0 on any error.
 * Each out_handles[i] is NUL-terminated. Handles exceeding MAX_MENTION_HANDLE_LEN
 * are silently skipped.
 */
#define MAX_HEADER_LINE 8192

static int read_chat_participants(const char *chat_path,
                                   char out_handles[][MAX_MENTION_HANDLE_LEN],
                                   int max_handles) {
    ASSERT_MSG(chat_path != NULL, "read_chat_participants: chat_path is NULL");
    ASSERT_MSG(out_handles != NULL, "read_chat_participants: out_handles is NULL");
    ASSERT_MSG(max_handles > 0, "read_chat_participants: max_handles must be positive, got %d", max_handles);

    FILE *f = fopen(chat_path, "r");
    if (!f) {
        fprintf(stderr, "warning: read_chat_participants: cannot open %s: %s\n",
                chat_path, strerror(errno));
        return 0;
    }

    char line[MAX_HEADER_LINE];
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';

        /* Stop at separator — header is over */
        if (strcmp(line, "---") == 0)
            break;

        /* Look for participants line */
        const char *prefix = "participants: ";
        size_t prefix_len = strlen(prefix);
        if (strncmp(line, prefix, prefix_len) != 0)
            continue;

        /* Parse "handle1(N1), handle2(N2), ..." */
        const char *p = line + prefix_len;
        while (*p != '\0' && found < max_handles) {
            /* Skip whitespace */
            while (*p == ' ')
                p++;
            if (*p == '\0')
                break;

            /* Extract handle (everything before '(') */
            const char *start = p;
            while (*p != '\0' && *p != '(')
                p++;

            size_t handle_len = (size_t)(p - start);
            /* Trim trailing whitespace (Violation 6 fix: prevents
             * sender-comparison mismatches in @team expansion) */
            while (handle_len > 0 && start[handle_len - 1] == ' ')
                handle_len--;
            if (handle_len > 0 && handle_len < MAX_MENTION_HANDLE_LEN) {
                memcpy(out_handles[found], start, handle_len);
                out_handles[found][handle_len] = '\0';
                found++;
            }

            /* Skip past "(N)" */
            if (*p == '(') {
                while (*p != '\0' && *p != ')')
                    p++;
                if (*p == ')')
                    p++;
            }

            /* Skip ", " separator */
            if (*p == ',')
                p++;
        }

        break; /* Only one participants line */
    }

    fclose(f);
    return found;
}

int bus_extract_mentions(const char *message,
                         char out_handles[][MAX_MENTION_HANDLE_LEN],
                         int max_handles,
                         int *out_interrupt_flags) {
    ASSERT_MSG(message != NULL,
               "bus_extract_mentions: message is NULL");
    ASSERT_MSG(out_handles != NULL,
               "bus_extract_mentions: out_handles is NULL");
    ASSERT_MSG(max_handles > 0,
               "bus_extract_mentions: max_handles must be positive, got %d",
               max_handles);

    int found = 0;
    const char *p = message;

    while (*p != '\0' && found < max_handles) {
        if (*p != '@') {
            p++;
            continue;
        }

        /* Found @. Check what precedes it. */
        if (p > message && is_email_prefix_char((unsigned char)*(p - 1))) {
            /* Preceded by email-like character — skip */
            p++;
            continue;
        }

        /* Extract the handle after @ */
        const char *start = p + 1;
        if (!is_handle_char((unsigned char)*start)) {
            /* @ followed by non-handle char — skip */
            p++;
            continue;
        }

        const char *end = start;
        while (is_handle_char((unsigned char)*end)) {
            end++;
        }

        size_t handle_len = (size_t)(end - start);
        if (handle_len == 0 || handle_len >= MAX_MENTION_HANDLE_LEN) {
            p = end;
            continue;
        }

        /* Check for duplicates */
        char candidate[MAX_MENTION_HANDLE_LEN];
        memcpy(candidate, start, handle_len);
        candidate[handle_len] = '\0';

        int is_dup = 0;
        for (int i = 0; i < found; i++) {
            if (strcmp(out_handles[i], candidate) == 0) {
                is_dup = 1;
                break;
            }
        }

        if (!is_dup) {
            memcpy(out_handles[found], candidate, handle_len + 1);
            /* Check for suffix: '!' (interrupt) or '?' (query) after handle.
             * Also accept '\!' and '\?' — LLMs frequently backslash-escape
             * these characters because markdown training primes them to
             * treat '!' as a special character. */
            if (out_interrupt_flags != NULL) {
                if (*end == '!' || (*end == '\\' && *(end + 1) == '!')) {
                    out_interrupt_flags[found] = 1;
                } else if (*end == '?' || (*end == '\\' && *(end + 1) == '?')) {
                    out_interrupt_flags[found] = 2;
                } else {
                    out_interrupt_flags[found] = 0;
                }
            }
            found++;
        }

        p = end;
    }

    /* Postcondition: found is in [0, max_handles] */
    ASSERT_MSG(found >= 0 && found <= max_handles,
               "bus_extract_mentions: found count %d out of range [0, %d]",
               found, max_handles);

    return found;
}

int bus_find_events_dir(const char *chat_path, char *out_buf,
                        size_t out_buf_size) {
    ASSERT_MSG(chat_path != NULL,
               "bus_find_events_dir: chat_path is NULL");
    ASSERT_MSG(out_buf != NULL,
               "bus_find_events_dir: out_buf is NULL");
    ASSERT_MSG(out_buf_size > 0,
               "bus_find_events_dir: out_buf_size must be positive");

    /*
     * Strategy: the chat file is at .nbs/chat/foo.chat.
     * The events dir is at .nbs/events/.
     * So from the chat file, go up two levels (to project root),
     * then down into .nbs/events/.
     *
     * We also handle the case where the chat file is at an arbitrary
     * location by walking up directories looking for .nbs/events/.
     */

    /* Make a mutable copy for dirname */
    char path_copy[MAX_PATH_LEN];
    size_t path_len = strlen(chat_path);
    if (path_len >= sizeof(path_copy)) {
        return -1;
    }
    memcpy(path_copy, chat_path, path_len + 1);

    /* Get the directory containing the chat file */
    char *dir = dirname(path_copy);

    /* Walk up looking for .nbs/events/ */
    char check_path[MAX_PATH_LEN];
    /*
     * prev_dir tracks the previous iteration's directory to detect reaching
     * the filesystem root (where dirname("/") == "/"). For relative paths
     * this check is weaker: dirname("a") == "." and dirname(".") == ".",
     * so it still terminates, but the intermediate traversal may not match
     * what the caller expects. In practice, chat_path is always absolute
     * because chat_file.c resolves it with realpath() before calling us.
     */
    char prev_dir[MAX_PATH_LEN] = "";

    for (int depth = 0; depth < MAX_DIR_WALK_DEPTH; depth++) {
        /* Check if <dir>/events/ exists (when dir is .nbs/chat/) */
        /* Actually, check <dir>/../events/ first (sibling of chat/) */
        snprintf(check_path, sizeof(check_path), "%s/../events", dir);

        /* Resolve the path */
        char resolved[MAX_PATH_LEN];
        if (realpath(check_path, resolved) != NULL) {
            struct stat st;
            if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (strlen(resolved) < out_buf_size) {
                    memcpy(out_buf, resolved, strlen(resolved) + 1);
                    ASSERT_MSG(out_buf[0] == '/',
                               "bus_find_events_dir: resolved path is not absolute: %s",
                               out_buf);
                    return 0;
                }
                return -1;  /* Path too long for output buffer */
            }
        }

        /* Also check <dir>/.nbs/events/ */
        snprintf(check_path, sizeof(check_path), "%s/.nbs/events", dir);
        if (realpath(check_path, resolved) != NULL) {
            struct stat st;
            if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (strlen(resolved) < out_buf_size) {
                    memcpy(out_buf, resolved, strlen(resolved) + 1);
                    ASSERT_MSG(out_buf[0] == '/',
                               "bus_find_events_dir: resolved path is not absolute: %s",
                               out_buf);
                    return 0;
                }
                return -1;
            }
        }

        /* Move up one directory */
        char dir_copy[MAX_PATH_LEN];
        snprintf(dir_copy, sizeof(dir_copy), "%s", dir);

        /* Prevent infinite loop at root */
        if (strcmp(dir_copy, prev_dir) == 0) {
            break;
        }
        snprintf(prev_dir, sizeof(prev_dir), "%s", dir_copy);

        /* dirname modifies its argument, so copy first */
        char up_copy[MAX_PATH_LEN];
        snprintf(up_copy, sizeof(up_copy), "%s", dir);
        dir = dirname(up_copy);
        /*
         * dirname() may return a pointer into up_copy or a static string
         * (e.g. "." or "/"). Either way, the pointer is only valid until
         * the next dirname() call. Copy immediately into path_copy to
         * decouple from up_copy's lifetime and dirname's internal state.
         */
        ASSERT_MSG(dir != NULL, "bus_find_events_dir: dirname returned NULL");
        snprintf(path_copy, sizeof(path_copy), "%s", dir);
        dir = path_copy;
    }

    return -1;  /* Not found */
}

/*
 * bus_publish — Execute nbs-bus publish with the given arguments.
 *
 * payload may be NULL (treated as empty string).
 * Returns 0 on success, -1 on failure. Failure is non-fatal to the caller.
 */
static int bus_publish(const char *events_dir, const char *source,
                       const char *type, const char *priority,
                       const char *payload) {
    ASSERT_MSG(events_dir != NULL, "bus_publish: events_dir is NULL");
    ASSERT_MSG(source != NULL, "bus_publish: source is NULL");
    ASSERT_MSG(type != NULL, "bus_publish: type is NULL");
    ASSERT_MSG(priority != NULL, "bus_publish: priority is NULL");

    /*
     * Build command: nbs-bus publish <dir> <source> <type> <priority> <payload>
     *
     * We use fork+exec rather than system() to avoid shell injection.
     * The payload is passed as a single argv element, not parsed by a shell.
     *
     * Payload truncation is done pre-fork so the child process only
     * calls async-signal-safe functions (open, dup2, close, execlp, _exit).
     */
    char truncated_payload[MAX_PAYLOAD_LEN];
    if (payload != NULL) {
        snprintf(truncated_payload, sizeof(truncated_payload), "%s", payload);
    } else {
        truncated_payload[0] = '\0';
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "bus_bridge: fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process — only async-signal-safe functions below. */

        /* Redirect stdout/stderr to /dev/null — bus output should not
         * interfere with chat output.
         * open/dup2/close are async-signal-safe per POSIX.1-2008. */
        int devnull_fd = open("/dev/null", O_WRONLY);
        if (devnull_fd >= 0) {
            dup2(devnull_fd, STDOUT_FILENO);
            dup2(devnull_fd, STDERR_FILENO);
            close(devnull_fd);
        } else {
            /* /dev/null unavailable (e.g. chroot). Close fds outright
             * so the child doesn't write to the parent's terminal. */
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        }

        const char *bus_bin = resolve_nbs_bus();
        execlp(bus_bin, "nbs-bus", "publish",
              events_dir, source, type, priority,
              truncated_payload, "--dedup-window=0", (char *)NULL);

        /* exec failed — exit silently */
        _exit(1);
    }

    /*
     * Parent: wait for child, but don't fail if it fails.
     *
     * SIGCHLD assumption: the caller has not set SIG_IGN for SIGCHLD.
     * If SIGCHLD is SIG_IGN, the child is auto-reaped and waitpid()
     * returns -1 with errno == ECHILD. The status variable would then
     * be uninitialised. We handle this below by checking the return.
     */
    int status;
    pid_t wpid = waitpid(pid, &status, 0);
    if (wpid < 0) {
        /* ECHILD: child already reaped (SIG_IGN) or doesn't exist.
         * Other errors (EINTR) are also non-fatal for the bus bridge. */
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }

    /* Bus publish failed — log but don't propagate */
    /* Don't log if exec failed (exit code 1 from _exit) — nbs-bus likely
     * not installed yet. Don't log dedup (exit code 5) — expected for
     * rapid sends. Only log unexpected failures. */
    if (WIFEXITED(status) && WEXITSTATUS(status) != 1
                          && WEXITSTATUS(status) != 5) {
        fprintf(stderr, "bus_bridge: nbs-bus publish exited with %d\n",
                WEXITSTATUS(status));
    }

    return -1;
}

int bus_bridge_after_send(const char *chat_path, const char *handle,
                          const char *message) {
    ASSERT_MSG(chat_path != NULL,
               "bus_bridge_after_send: chat_path is NULL");
    ASSERT_MSG(handle != NULL && handle[0] != '\0',
               "bus_bridge_after_send: handle is NULL or empty");
    ASSERT_MSG(message != NULL,
               "bus_bridge_after_send: message is NULL");
    /* Empty messages are valid user input — skip bus event, don't crash */
    if (message[0] == '\0')
        return 0;

    /* Find the events directory */
    char events_dir[MAX_PATH_LEN];
    if (bus_find_events_dir(chat_path, events_dir, sizeof(events_dir)) != 0) {
        /* No bus directory — silently return */
        return 0;
    }

    /* Build payload: "handle: message" */
    char payload[MAX_PAYLOAD_LEN];
    snprintf(payload, sizeof(payload), "%s: %s", handle, message);

    /* Publish chat-message event */
    bus_publish(events_dir, "nbs-chat", "chat-message", "normal", payload);

    /* Check for @mentions */
    char mentions[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int interrupt_flags[MAX_MENTIONS];
    int mention_count = bus_extract_mentions(message, mentions, MAX_MENTIONS,
                                             interrupt_flags);

    /* Publish chat-mention, chat-interrupt, or chat-query events */
    for (int i = 0; i < mention_count; i++) {
        /* Determine event type and priority from interrupt flag */
        const char *event_type;
        const char *priority;
        if (interrupt_flags[i] == 1) {
            event_type = "chat-interrupt";
            priority = "critical";
        } else if (interrupt_flags[i] == 2) {
            event_type = "chat-query";
            priority = "high";
        } else {
            event_type = "chat-mention";
            priority = "high";
        }

        if (strcmp(mentions[i], "team") == 0) {
            /*
             * @team expansion: read participants from the chat file header
             * and publish one event per participant (excluding the sender).
             * This ensures every sidecar gets its own individually-addressable
             * event that it can ack independently.
             */
            char participants[MAX_PARTICIPANTS][MAX_MENTION_HANDLE_LEN];
            int pcount = read_chat_participants(chat_path, participants,
                                                 MAX_PARTICIPANTS);
            for (int j = 0; j < pcount; j++) {
                /* Skip the sender — don't mention yourself */
                if (strcmp(participants[j], handle) == 0)
                    continue;
                /* Skip sidecar — not a team member */
                if (strcmp(participants[j], "sidecar") == 0)
                    continue;

                /* Copy handle to local buffer — GCC cannot infer
                 * element size of 2D VLA for format-truncation analysis */
                char participant[MAX_MENTION_HANDLE_LEN];
                size_t plen = strlen(participants[j]);
                if (plen >= sizeof(participant))
                    plen = sizeof(participant) - 1;
                memcpy(participant, participants[j], plen);
                participant[plen] = '\0';

                char mention_payload[MAX_PAYLOAD_LEN];
                snprintf(mention_payload, sizeof(mention_payload),
                         "@%s from %s: %s", participant, handle, message);
                bus_publish(events_dir, "nbs-chat", event_type, priority,
                            mention_payload);
            }
        } else {
            /* Single-handle mention/interrupt/query */
            char mention_payload[MAX_PAYLOAD_LEN];
            snprintf(mention_payload, sizeof(mention_payload),
                     "@%s from %s: %s", mentions[i], handle, message);
            bus_publish(events_dir, "nbs-chat", event_type, priority,
                        mention_payload);
        }
    }

    /* Postcondition: always returns 0 — bus bridge never fails */
    return 0;
}

int bus_bridge_human_input(const char *chat_path, const char *handle,
                            const char *message) {
    ASSERT_MSG(chat_path != NULL,
               "bus_bridge_human_input: chat_path is NULL");
    ASSERT_MSG(handle != NULL && handle[0] != '\0',
               "bus_bridge_human_input: handle is NULL or empty");
    ASSERT_MSG(message != NULL,
               "bus_bridge_human_input: message is NULL");
    /* Empty messages are valid user input — skip bus event, don't crash */
    if (message[0] == '\0')
        return 0;

    /* Find the events directory */
    char events_dir[MAX_PATH_LEN];
    if (bus_find_events_dir(chat_path, events_dir, sizeof(events_dir)) != 0) {
        /* No bus directory — silently return */
        return 0;
    }

    /* Build payload: "handle: message" */
    char payload[MAX_PAYLOAD_LEN];
    snprintf(payload, sizeof(payload), "%s: %s", handle, message);

    /* Publish human-input event at high priority */
    bus_publish(events_dir, "nbs-chat-terminal", "human-input", "high", payload);

    /* Postcondition: always returns 0 — bus bridge never fails */
    return 0;
}
