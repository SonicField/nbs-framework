/*
 * chat_client.c — Chat file operations for the sidecar.
 *
 * Ported from the bash implementation in bin/nbs-claude:
 *   check_chat_unread              → chat_client_check_unread
 *   are_chat_unread_sidecar_only   → chat_client_are_unread_sidecar_only
 *
 * Direct file I/O for read-only operations (message counting, cursor
 * reading). Uses nbs-chat send via fork+exec for posting messages.
 * No locks are acquired for read-only operations, matching bash behaviour.
 *
 * Invariants:
 *   - All string construction uses snprintf with bounded buffers
 *   - Message counting and cursor reading are lock-free (read-only)
 *   - Base64 decoding uses the shared nbs-chat base64 module
 *   - chat_client_send delegates to exec_fire_and_forget
 */

#include "chat_client.h"
#include "exec_util.h"
#include "registry.h"
#include "../nbs-common/nbs_assert.h"
#include "../nbs-chat/base64.h"

#include <stdio.h>
#include <string.h>
#include <libgen.h>
#include <unistd.h>

#define MAX_LINE 4096
#define MAX_DECODED 8192

/*
 * Cached absolute path to the nbs-chat binary.
 * Resolved once via /proc/self/exe — sibling binary in same directory.
 */
#define CHAT_PATH_LEN 4096
static char nbs_chat_path[CHAT_PATH_LEN] = "";
static int nbs_chat_path_resolved = 0;

static const char *resolve_nbs_chat(void)
{
    if (nbs_chat_path_resolved)
        return nbs_chat_path[0] ? nbs_chat_path : "nbs-chat";

    nbs_chat_path_resolved = 1;

    char self[CHAT_PATH_LEN];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len <= 0)
        return "nbs-chat";
    self[len] = '\0';

    char *slash = strrchr(self, '/');
    if (!slash)
        return "nbs-chat";

    size_t dir_len = (size_t)(slash - self);
    if (dir_len + sizeof("/nbs-chat") > sizeof(nbs_chat_path))
        return "nbs-chat";

    memcpy(nbs_chat_path, self, dir_len);
    memcpy(nbs_chat_path + dir_len, "/nbs-chat", sizeof("/nbs-chat"));

    if (access(nbs_chat_path, X_OK) != 0) {
        nbs_chat_path[0] = '\0';
        return "nbs-chat";
    }

    return nbs_chat_path;
}

/* ---- chat_client_count_messages ---- */

int chat_client_count_messages(const char *chat_path)
{
    ASSERT_MSG(chat_path != NULL, "chat_client_count_messages: chat_path is NULL");
    ASSERT_MSG(chat_path[0] != '\0', "chat_client_count_messages: chat_path is empty");

    FILE *f = fopen(chat_path, "r");
    if (!f)
        return -1;

    char line[MAX_LINE];
    int found_separator = 0;
    int count = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        len = strlen(line);

        if (!found_separator) {
            if (strcmp(line, "---") == 0)
                found_separator = 1;
            continue;
        }

        /* After separator: count non-empty lines */
        if (len > 0)
            count++;
    }

    fclose(f);
    return count;
}

/* ---- chat_client_read_cursor ---- */

int chat_client_read_cursor(const char *chat_path, const char *handle)
{
    ASSERT_MSG(chat_path != NULL, "chat_client_read_cursor: chat_path is NULL");
    ASSERT_MSG(handle != NULL, "chat_client_read_cursor: handle is NULL");
    ASSERT_MSG(handle[0] != '\0', "chat_client_read_cursor: handle is empty");

    char cursor_path[MAX_LINE];
    int n = snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
    ASSERT_MSG(n >= 0 && (size_t)n < sizeof(cursor_path),
               "chat_client: cursor path truncated");

    FILE *f = fopen(cursor_path, "r");
    if (!f)
        return -1;

    char line[MAX_LINE];
    int result = -1;

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        /* Skip comments */
        if (line[0] == '#')
            continue;

        /* Skip empty lines */
        if (line[0] == '\0')
            continue;

        /* Split on '=' */
        char *eq = strchr(line, '=');
        if (!eq)
            continue;

        *eq = '\0';
        const char *key = line;
        const char *value = eq + 1;

        if (strcmp(key, handle) == 0) {
            int parsed = 0;
            if (sscanf(value, "%d", &parsed) == 1)
                result = parsed;
            break;
        }
    }

    fclose(f);
    return result;
}

/* ---- chat_client_check_unread callback ---- */

/*
 * Callback context for registry_for_each when checking unread messages.
 */
struct check_unread_ctx {
    const char *handle;
    int unread_count;
    int has_chat;
    char *summary;
    size_t sum_size;
    size_t sum_used; /* bytes written so far into summary (excluding NUL) */
};

/*
 * check_unread_cb — Callback for each chat: registry entry.
 *
 * Counts unread messages for a single chat file and appends to
 * the human-readable summary. Always returns 0 to continue iteration.
 */
static int check_unread_cb(const char *path, void *user_data)
{
    struct check_unread_ctx *ctx = user_data;

    int total = chat_client_count_messages(path);
    if (total < 0)
        return 0; /* File missing or unreadable — skip, matching bash */

    ctx->has_chat = 1;

    int cursor = chat_client_read_cursor(path, ctx->handle);
    if (cursor < 0)
        cursor = 0;

    /* Unread if total > cursor + 1 (cursor is 0-indexed last-read index) */
    if (total > cursor + 1) {
        int n_unread = total - cursor - 1;
        ctx->unread_count += n_unread;

        /* Extract basename for summary.
         * basename() may modify its argument, so work on a copy. */
        char path_copy[MAX_LINE];
        int n = snprintf(path_copy, sizeof(path_copy), "%s", path);
        ASSERT_MSG(n >= 0 && (size_t)n < sizeof(path_copy),
                   "chat_client: path copy truncated");
        const char *name = basename(path_copy);

        /* Append to summary: "N unread in file1, file2" */
        if (ctx->sum_used > 0 && ctx->sum_used < ctx->sum_size - 2) {
            int written = snprintf(ctx->summary + ctx->sum_used,
                                   ctx->sum_size - ctx->sum_used,
                                   ", %s", name);
            if (written >= 0 && (size_t)written < ctx->sum_size - ctx->sum_used) {
                ctx->sum_used += (size_t)written;
            }
            /* else: truncated — stop appending, summary is already NUL-terminated */
        } else if (ctx->sum_used == 0) {
            /* First entry — just store the name, prefix added later */
            int written = snprintf(ctx->summary + ctx->sum_used,
                                   ctx->sum_size - ctx->sum_used,
                                   "%s", name);
            if (written >= 0 && (size_t)written < ctx->sum_size - ctx->sum_used) {
                ctx->sum_used += (size_t)written;
            }
            /* else: truncated — stop appending, summary is already NUL-terminated */
        }
    }

    return 0;
}

/* ---- chat_client_check_unread ---- */

int chat_client_check_unread(const char *registry_path, const char *handle,
                              int *unread_count, char *summary, size_t sum_size)
{
    ASSERT_MSG(registry_path != NULL,
               "chat_client_check_unread: registry_path is NULL");
    ASSERT_MSG(handle != NULL,
               "chat_client_check_unread: handle is NULL");
    ASSERT_MSG(unread_count != NULL,
               "chat_client_check_unread: unread_count is NULL");
    ASSERT_MSG(summary != NULL,
               "chat_client_check_unread: summary is NULL");
    ASSERT_MSG(sum_size > 0,
               "chat_client_check_unread: sum_size is 0");

    *unread_count = 0;
    summary[0] = '\0';

    struct check_unread_ctx ctx = {
        .handle = handle,
        .unread_count = 0,
        .has_chat = 0,
        .summary = summary,
        .sum_size = sum_size,
        .sum_used = 0,
    };

    int rc = registry_for_each(registry_path, "chat", check_unread_cb, &ctx);
    if (rc < 0)
        return -1;

    *unread_count = ctx.unread_count;

    /* Build final summary: "N unread in file1, file2" */
    if (ctx.unread_count > 0) {
        /* summary currently holds "file1, file2" — prepend count.
         * Copy current content aside, then rebuild. */
        char chat_names[MAX_LINE];
        int n = snprintf(chat_names, sizeof(chat_names), "%s", summary);
        ASSERT_MSG(n >= 0 && (size_t)n < sizeof(chat_names),
                   "chat_client: chat names truncated");
        n = snprintf(summary, sum_size, "%d unread in %s",
                     ctx.unread_count, chat_names);
        ASSERT_MSG(n >= 0 && (size_t)n < sum_size,
                   "chat_client: final summary truncated");
    }

    if (!ctx.has_chat)
        return 2;
    if (ctx.unread_count == 0)
        return 1;
    return 0;
}

/* ---- chat_client_are_unread_sidecar_only callback ---- */

/*
 * Callback context for checking whether all unread messages are from sidecar.
 */
struct sidecar_only_ctx {
    const char *handle;
    int has_unread;
    int found_non_sidecar;
};

/*
 * extract_handle_from_decoded — Extract the sender handle from a decoded message.
 *
 * Decoded message format:
 *   "handle|epoch: content"   (timestamped)
 *   "handle: content"         (legacy)
 *
 * Extracts the handle (before first "|" or ": ") and writes it to out_handle.
 * Returns 0 on success, -1 if no handle delimiter found.
 */
static int extract_handle_from_decoded(const char *decoded, size_t decoded_len,
                                       char *out_handle, size_t handle_size)
{
    ASSERT_MSG(handle_size > 0, "extract_handle_from_decoded: handle_size is 0");

    /* Find first ": " — this separates handle (possibly with |epoch) from content */
    const char *colon_space = NULL;
    for (size_t i = 0; i + 1 < decoded_len; i++) {
        if (decoded[i] == ':' && decoded[i + 1] == ' ') {
            colon_space = decoded + i;
            break;
        }
    }

    if (!colon_space)
        return -1;

    /* Everything before ": " is handle_part, which may be "handle|epoch" */
    size_t handle_part_len = (size_t)(colon_space - decoded);

    /* Find '|' within handle_part to strip timestamp suffix */
    size_t handle_len = handle_part_len;
    for (size_t i = 0; i < handle_part_len; i++) {
        if (decoded[i] == '|') {
            handle_len = i;
            break;
        }
    }

    if (handle_len >= handle_size)
        handle_len = handle_size - 1;

    memcpy(out_handle, decoded, handle_len);
    out_handle[handle_len] = '\0';

    return 0;
}

/*
 * sidecar_only_cb — Callback for each chat: registry entry.
 *
 * For each chat with unread messages, decodes each unread message
 * and checks if the sender handle is "sidecar".
 * Returns non-zero to stop iteration early if a non-sidecar message is found.
 */
static int sidecar_only_cb(const char *path, void *user_data)
{
    struct sidecar_only_ctx *ctx = user_data;

    int total = chat_client_count_messages(path);
    if (total < 0)
        return 0;

    int cursor = chat_client_read_cursor(path, ctx->handle);
    if (cursor < 0)
        cursor = 0;

    /* No unread in this chat — skip */
    if (total <= cursor + 1)
        return 0;

    ctx->has_unread = 1;

    /* Open chat file and find unread messages */
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char line[MAX_LINE];
    int found_separator = 0;
    int msg_index = 0;          /* 0-based index of messages after --- */
    int skip_count = cursor + 1; /* messages 0..cursor have been read */

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        len = strlen(line);

        if (!found_separator) {
            if (strcmp(line, "---") == 0)
                found_separator = 1;
            continue;
        }

        /* Skip empty lines (they don't count as messages) */
        if (len == 0)
            continue;

        msg_index++;

        /* Skip already-read messages */
        if (msg_index <= skip_count)
            continue;

        /* Decode base64 message line */
        unsigned char decoded[MAX_DECODED];
        int decoded_len = base64_decode(line, len, decoded, sizeof(decoded));
        if (decoded_len < 0)
            continue; /* Decode failure — skip */

        /* NUL-terminate for string operations */
        if ((size_t)decoded_len < sizeof(decoded))
            decoded[decoded_len] = '\0';
        else
            decoded[sizeof(decoded) - 1] = '\0';

        /* Extract handle */
        char msg_handle[MAX_LINE];
        if (extract_handle_from_decoded((const char *)decoded, (size_t)decoded_len,
                                        msg_handle, sizeof(msg_handle)) != 0)
            continue; /* Can't parse handle — skip */

        if (strcmp(msg_handle, "sidecar") != 0) {
            ctx->found_non_sidecar = 1;
            fclose(f);
            return 1; /* Stop iteration — found non-sidecar message */
        }
    }

    fclose(f);
    return 0;
}

/* ---- chat_client_are_unread_sidecar_only ---- */

int chat_client_are_unread_sidecar_only(const char *registry_path,
                                         const char *handle)
{
    ASSERT_MSG(registry_path != NULL,
               "chat_client_are_unread_sidecar_only: registry_path is NULL");
    ASSERT_MSG(handle != NULL,
               "chat_client_are_unread_sidecar_only: handle is NULL");

    struct sidecar_only_ctx ctx = {
        .handle = handle,
        .has_unread = 0,
        .found_non_sidecar = 0,
    };

    int rc = registry_for_each(registry_path, "chat", sidecar_only_cb, &ctx);
    if (rc < 0)
        return 0; /* Error — conservatively treat as non-sidecar */

    /* All unread are sidecar-only: has_unread && !found_non_sidecar */
    if (ctx.has_unread && !ctx.found_non_sidecar)
        return 1;

    return 0;
}

/* ---- chat_client_send ---- */

int chat_client_send(const char *chat_path, const char *handle,
                      const char *message)
{
    ASSERT_MSG(chat_path != NULL, "chat_client_send: chat_path is NULL");
    ASSERT_MSG(handle != NULL, "chat_client_send: handle is NULL");
    ASSERT_MSG(message != NULL, "chat_client_send: message is NULL");

    const char *argv[] = {
        resolve_nbs_chat(), "send", chat_path, handle, message, NULL
    };

    int rc = exec_fire_and_forget(argv);
    return (rc == 0) ? 0 : -1;
}
