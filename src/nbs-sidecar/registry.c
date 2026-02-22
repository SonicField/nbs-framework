/*
 * registry.c — Control inbox/registry management.
 *
 * Ported from the bash implementation in bin/nbs-claude:
 *   seed_registry      → registry_seed
 *   process_control_command + check_control_inbox → registry_process_inbox
 *
 * The registry file is a simple line-oriented format: type:path
 * The control inbox is: verb path (one per line, # comments, empty lines skipped)
 *
 * Invariants:
 *   - registry_seed is idempotent (never adds duplicates)
 *   - registry_process_inbox is forward-only (never re-processes old lines)
 *   - Unregister uses atomic tmp+rename to avoid partial writes
 *   - All string construction uses snprintf with bounded buffers
 *
 * Buffer sizing rationale (satisfies -Werror=format-truncation):
 *   MAX_PATH  (4096) — individual path components, nbs_root, inbox lines
 *   MAX_FPATH (4608) — assembled file paths (dir + "/" + filename)
 *   MAX_ENTRY (4624) — registry entries ("chat:" + full path)
 */

#include "registry.h"
#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define MAX_PATH  4096
#define MAX_FPATH (MAX_PATH + 512)
#define MAX_ENTRY (MAX_FPATH + 16)

/* ---- Static helpers ---- */

/*
 * registry_contains — Check if registry file contains a given line.
 *
 * Returns 1 if the exact line (as a complete line) is found, 0 otherwise.
 * Returns 0 if the file does not exist or cannot be read.
 */
static int registry_contains(const char *registry_path, const char *entry)
{
    ASSERT_MSG(registry_path != NULL, "registry_contains: registry_path is NULL");
    ASSERT_MSG(entry != NULL, "registry_contains: entry is NULL");

    FILE *f = fopen(registry_path, "r");
    if (!f)
        return 0;

    char line[MAX_ENTRY];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (strcmp(line, entry) == 0) {
            fclose(f);
            return 1;
        }
    }

    fclose(f);
    return 0;
}

/*
 * registry_append — Append a single line to the registry file.
 *
 * Creates the file if it does not exist.
 * Returns 0 on success, -1 on error.
 */
static int registry_append(const char *registry_path, const char *entry)
{
    ASSERT_MSG(registry_path != NULL, "registry_append: registry_path is NULL");
    ASSERT_MSG(entry != NULL, "registry_append: entry is NULL");

    FILE *f = fopen(registry_path, "a");
    if (!f)
        return -1;

    if (fprintf(f, "%s\n", entry) < 0) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

/*
 * registry_remove — Remove all lines matching entry from registry.
 *
 * Writes to a temporary file, then renames over the original (atomic).
 * Returns 0 on success, -1 on error.
 */
static int registry_remove(const char *registry_path, const char *entry)
{
    ASSERT_MSG(registry_path != NULL, "registry_remove: registry_path is NULL");
    ASSERT_MSG(entry != NULL, "registry_remove: entry is NULL");

    FILE *f = fopen(registry_path, "r");
    if (!f)
        return (errno == ENOENT) ? 0 : -1;

    char tmp_path[MAX_FPATH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", registry_path);

    FILE *tmp = fopen(tmp_path, "w");
    if (!tmp) {
        fclose(f);
        return -1;
    }

    char line[MAX_ENTRY];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline for comparison */
        size_t len = strlen(line);
        char stripped[MAX_ENTRY];
        memcpy(stripped, line, len + 1);
        if (len > 0 && stripped[len - 1] == '\0')
            stripped[len - 1] = '\0';

        if (strcmp(stripped, entry) != 0) {
            if (fputs(line, tmp) == EOF) {
                fclose(f);
                fclose(tmp);
                unlink(tmp_path);
                return -1;
            }
        }
    }

    fclose(f);
    fclose(tmp);

    if (rename(tmp_path, registry_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

/*
 * str_ends_with — Check if s ends with suffix.
 */
static int str_ends_with(const char *s, const char *suffix)
{
    ASSERT_MSG(s != NULL, "str_ends_with: s is NULL");
    ASSERT_MSG(suffix != NULL, "str_ends_with: suffix is NULL");

    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (suflen > slen)
        return 0;
    return strcmp(s + slen - suflen, suffix) == 0;
}

/*
 * strip_whitespace — Strip leading and trailing whitespace in-place.
 *
 * Returns pointer into the same buffer (possibly offset from start).
 * Modifies the buffer by NUL-terminating at the trailing whitespace boundary.
 */
static char *strip_whitespace(char *s)
{
    ASSERT_MSG(s != NULL, "strip_whitespace: s is NULL");

    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\r')
        s++;

    /* Trim trailing whitespace */
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }

    return s;
}

/*
 * count_lines — Count total lines in a buffer.
 *
 * A line is a sequence of characters terminated by '\n'.
 * A final line without '\n' still counts as a line (if non-empty).
 */
static int count_lines(const char *buf)
{
    if (!buf || *buf == '\0')
        return 0;

    int count = 0;
    const char *p = buf;
    while (*p) {
        if (*p == '\n')
            count++;
        p++;
    }

    /* If the buffer doesn't end with '\n', count the last line */
    if (p > buf && *(p - 1) != '\n')
        count++;

    return count;
}

/*
 * get_line_n — Get the nth line (0-indexed) from a buffer.
 *
 * Copies the line (without trailing newline) into out.
 * Returns 0 on success, -1 if line index is out of range.
 */
static int get_line_n(const char *buf, int n, char *out, size_t out_size)
{
    ASSERT_MSG(buf != NULL, "get_line_n: buf is NULL");
    ASSERT_MSG(out != NULL, "get_line_n: out is NULL");
    ASSERT_MSG(out_size > 0, "get_line_n: out_size is 0");

    int current = 0;
    const char *p = buf;

    while (*p && current < n) {
        if (*p == '\n')
            current++;
        p++;
    }

    if (current != n || *p == '\0')
        return -1;

    /* Copy until newline or end of string */
    size_t i = 0;
    while (*p && *p != '\n' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';

    return 0;
}

/*
 * process_control_command — Handle a single control command line.
 *
 * Precondition: line is non-empty and stripped.
 * Postcondition: registry is updated if command is valid.
 * Returns 0 on success (including for unknown commands), -1 on I/O error.
 */
static int process_control_command(const char *line, const char *registry_path)
{
    ASSERT_MSG(line != NULL, "process_control_command: line is NULL");
    ASSERT_MSG(registry_path != NULL, "process_control_command: registry_path is NULL");

    char verb[256];
    char path[MAX_PATH];

    /* Extract verb and path */
    int matched = sscanf(line, "%255s %4095s", verb, path);
    if (matched < 2)
        return 0; /* Incomplete line — ignore, matching bash behaviour */

    char entry[MAX_ENTRY];

    if (strcmp(verb, "register-chat") == 0) {
        snprintf(entry, sizeof(entry), "chat:%s", path);
        if (!registry_contains(registry_path, entry))
            return registry_append(registry_path, entry);
    } else if (strcmp(verb, "unregister-chat") == 0) {
        snprintf(entry, sizeof(entry), "chat:%s", path);
        return registry_remove(registry_path, entry);
    } else if (strcmp(verb, "register-bus") == 0) {
        snprintf(entry, sizeof(entry), "bus:%s", path);
        if (!registry_contains(registry_path, entry))
            return registry_append(registry_path, entry);
    } else if (strcmp(verb, "unregister-bus") == 0) {
        snprintf(entry, sizeof(entry), "bus:%s", path);
        return registry_remove(registry_path, entry);
    }
    /* Unknown verb — silently ignore, matching bash behaviour */

    return 0;
}

/* ---- Public API ---- */

int registry_seed(const char *nbs_root, const char *registry_path)
{
    ASSERT_MSG(nbs_root != NULL, "registry_seed: nbs_root is NULL");
    ASSERT_MSG(registry_path != NULL, "registry_seed: registry_path is NULL");

    /* Ensure registry file exists */
    FILE *f = fopen(registry_path, "a");
    if (!f)
        return -1;
    fclose(f);

    /* Scan <nbs_root>/.nbs/chat/ for .chat files */
    char chat_dir[MAX_PATH];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", nbs_root);

    DIR *dir = opendir(chat_dir);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;
            if (!str_ends_with(ent->d_name, ".chat"))
                continue;

            /* chat_dir (<=4095) + "/" + d_name (<=255) fits in MAX_FPATH */
            char full_path[MAX_FPATH];
            snprintf(full_path, sizeof(full_path), "%s/%s",
                     chat_dir, ent->d_name);

            /* Verify it's a regular file */
            struct stat st;
            if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode))
                continue;

            /* "chat:" (5) + full_path (<=MAX_FPATH-1) fits in MAX_ENTRY */
            char entry[MAX_ENTRY];
            snprintf(entry, sizeof(entry), "chat:%s", full_path);

            if (!registry_contains(registry_path, entry)) {
                if (registry_append(registry_path, entry) != 0) {
                    closedir(dir);
                    return -1;
                }
            }
        }
        closedir(dir);
    }

    /* Check <nbs_root>/.nbs/events directory */
    char events_dir[MAX_PATH];
    snprintf(events_dir, sizeof(events_dir), "%s/.nbs/events", nbs_root);

    struct stat st;
    if (stat(events_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        char entry[MAX_ENTRY];
        snprintf(entry, sizeof(entry), "bus:%s", events_dir);

        if (!registry_contains(registry_path, entry)) {
            if (registry_append(registry_path, entry) != 0)
                return -1;
        }
    }

    return 0;
}

int registry_process_inbox(const char *inbox_path, const char *registry_path,
                            int *inbox_line)
{
    ASSERT_MSG(inbox_path != NULL, "registry_process_inbox: inbox_path is NULL");
    ASSERT_MSG(registry_path != NULL,
               "registry_process_inbox: registry_path is NULL");
    ASSERT_MSG(inbox_line != NULL,
               "registry_process_inbox: inbox_line is NULL");

    /* Read entire inbox file */
    FILE *f = fopen(inbox_path, "r");
    if (!f)
        return (errno == ENOENT) ? 0 : -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long file_size = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    if (file_size <= 0) {
        fclose(f);
        return 0;
    }

    char *buf = malloc((size_t)file_size + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t nread = fread(buf, 1, (size_t)file_size, f);
    fclose(f);
    buf[nread] = '\0';

    int total_lines = count_lines(buf);

    if (total_lines <= *inbox_line) {
        free(buf);
        return 0;
    }

    int commands_processed = 0;

    for (int i = *inbox_line; i < total_lines; i++) {
        char line[MAX_PATH];
        if (get_line_n(buf, i, line, sizeof(line)) != 0)
            continue;

        char *stripped = strip_whitespace(line);

        /* Skip empty lines */
        if (*stripped == '\0')
            continue;

        /* Skip comments */
        if (*stripped == '#')
            continue;

        int cmd_rc = process_control_command(stripped, registry_path);
        if (cmd_rc < 0) {
            free(buf);
            return -1;
        }
        if (cmd_rc == 0)
            commands_processed++;
    }

    *inbox_line = total_lines;

    free(buf);
    return commands_processed;
}

int registry_find_first(const char *registry_path, const char *type,
                         char *out, size_t out_size)
{
    ASSERT_MSG(registry_path != NULL,
               "registry_find_first: registry_path is NULL");
    ASSERT_MSG(type != NULL, "registry_find_first: type is NULL");
    ASSERT_MSG(out != NULL, "registry_find_first: out is NULL");
    ASSERT_MSG(out_size > 0, "registry_find_first: out_size is 0");

    FILE *f = fopen(registry_path, "r");
    if (!f)
        return -1;

    char prefix[256];
    snprintf(prefix, sizeof(prefix), "%s:", type);
    size_t prefix_len = strlen(prefix);

    char line[MAX_ENTRY];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (strncmp(line, prefix, prefix_len) == 0) {
            snprintf(out, out_size, "%s", line + prefix_len);
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return -1;
}

int registry_for_each(const char *registry_path, const char *type,
                       int (*callback)(const char *path, void *user_data),
                       void *user_data)
{
    ASSERT_MSG(registry_path != NULL,
               "registry_for_each: registry_path is NULL");
    ASSERT_MSG(type != NULL, "registry_for_each: type is NULL");
    ASSERT_MSG(callback != NULL, "registry_for_each: callback is NULL");

    FILE *f = fopen(registry_path, "r");
    if (!f)
        return -1;

    char prefix[256];
    snprintf(prefix, sizeof(prefix), "%s:", type);
    size_t prefix_len = strlen(prefix);

    char line[MAX_ENTRY];
    int count = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (strncmp(line, prefix, prefix_len) == 0) {
            int rc = callback(line + prefix_len, user_data);
            count++;
            if (rc != 0) {
                fclose(f);
                return count;
            }
        }
    }

    fclose(f);
    return count;
}
