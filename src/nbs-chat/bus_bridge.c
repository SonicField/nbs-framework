/*
 * bus_bridge.c — Chat-to-bus event bridge implementation
 *
 * Publishes bus events via the nbs-bus binary when .nbs/events/ exists.
 * All bus failures are non-fatal — chat_send must never fail due to bus issues.
 */

#include "bus_bridge.h"

#include <ctype.h>
#include <errno.h>
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
            /* Check for interrupt suffix: '!' immediately after handle */
            if (out_interrupt_flags != NULL) {
                out_interrupt_flags[found] = (*end == '!') ? 1 : 0;
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
     */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "bus_bridge: fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process */
        /* Truncate payload if too long */
        char truncated_payload[MAX_PAYLOAD_LEN];
        if (payload != NULL) {
            snprintf(truncated_payload, sizeof(truncated_payload), "%s", payload);
        } else {
            truncated_payload[0] = '\0';
        }

        /* Redirect stdout/stderr to /dev/null — bus output should not
         * interfere with chat output */
        FILE *devnull = fopen("/dev/null", "w");
        if (devnull) {
            dup2(fileno(devnull), STDOUT_FILENO);
            dup2(fileno(devnull), STDERR_FILENO);
            fclose(devnull);
        } else {
            /* /dev/null unavailable (e.g. chroot). Close fds outright
             * so the child doesn't write to the parent's terminal. */
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        }

        execlp("nbs-bus", "nbs-bus", "publish",
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
    ASSERT_MSG(handle != NULL,
               "bus_bridge_after_send: handle is NULL");
    ASSERT_MSG(message != NULL,
               "bus_bridge_after_send: message is NULL");

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

    /* Publish chat-mention or chat-interrupt events for each @handle found */
    for (int i = 0; i < mention_count; i++) {
        char mention_payload[MAX_PAYLOAD_LEN];
        snprintf(mention_payload, sizeof(mention_payload),
                 "@%s from %s: %s", mentions[i], handle, message);
        if (interrupt_flags[i]) {
            /* @handle! — interrupt pattern: critical priority */
            bus_publish(events_dir, "nbs-chat", "chat-interrupt", "critical",
                        mention_payload);
        } else {
            /* @handle — normal mention: high priority */
            bus_publish(events_dir, "nbs-chat", "chat-mention", "high",
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
    ASSERT_MSG(handle != NULL,
               "bus_bridge_human_input: handle is NULL");
    ASSERT_MSG(message != NULL,
               "bus_bridge_human_input: message is NULL");

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
