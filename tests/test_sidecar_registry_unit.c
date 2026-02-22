/*
 * test_sidecar_registry_unit.c — Unit tests for registry management.
 *
 * Falsifiable claims tested:
 *   1.  seed: creates correct entries from .chat files and events dir
 *   2.  seed idempotent: calling twice does not create duplicates
 *   3.  find_first chat: returns path of first chat entry
 *   4.  find_first bus: returns path of bus entry
 *   5.  find_first nonexistent type: returns -1
 *   6.  for_each: counts entries correctly
 *   7.  process_inbox register-chat: adds new entry
 *   8.  process_inbox unregister-chat: removes entry
 *   9.  process_inbox forward-only: only processes new lines
 *  10.  process_inbox comments skipped
 *  11.  process_inbox empty lines skipped
 */

#include "../src/nbs-sidecar/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/*
 * Buffer tiers (satisfies -Werror=format-truncation):
 *   L0 (256) — tmpdir base, nbs_root
 *   L1 (512) — subdirectories (nbs_root + "/.nbs/chat")
 *   L2 (768) — file paths (L1 + "/filename.chat")
 *   L3 (1024)— registry entries ("chat:" + L2)
 */
#define L0 256
#define L1 512
#define L2 768
#define L3 1024

static int tests = 0, fails = 0;

#define CHECK(label, cond) do { \
    tests++; \
    if (!(cond)) { \
        fails++; \
        printf("   FAIL: %s\n", label); \
    } else { \
        printf("   PASS: %s\n", label); \
    } \
} while(0)

/* ---- Helpers ---- */

static void mkdirs(const char *path)
{
    char tmp[L2];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void touch(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

static int count_file_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int count = 0;
    char line[L3];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (strlen(line) > 0)
            count++;
    }
    fclose(f);
    return count;
}

static int file_contains_line(const char *path, const char *target)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[L3];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (strcmp(line, target) == 0) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static void rmrf(const char *path)
{
    char cmd[L1];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

/* ---- for_each callback ---- */

static int count_callback(const char *path, void *user_data)
{
    (void)path;
    int *count = (int *)user_data;
    (*count)++;
    return 0;
}

/* ---- Tests ---- */

int main(void)
{
    printf("test_sidecar_registry_unit\n");

    /* Create temp directory */
    char tmpdir[] = "/tmp/nbs_reg_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        fprintf(stderr, "Failed to create temp dir: %s\n", strerror(errno));
        return 1;
    }

    /* L0: tmpdir (~30 chars), nbs_root (~38 chars) */
    char nbs_root[L0];
    snprintf(nbs_root, sizeof(nbs_root), "%s/project", tmpdir);

    /* L1: subdirs */
    char chat_dir[L1];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", nbs_root);
    mkdirs(chat_dir);

    char events_dir[L1];
    snprintf(events_dir, sizeof(events_dir), "%s/.nbs/events", nbs_root);
    mkdirs(events_dir);

    char registry_path[L1];
    snprintf(registry_path, sizeof(registry_path), "%s/registry", tmpdir);

    char inbox_path[L1];
    snprintf(inbox_path, sizeof(inbox_path), "%s/inbox", tmpdir);

    /* L2: file paths */
    char chat1[L2], chat2[L2];
    snprintf(chat1, sizeof(chat1), "%s/live.chat", chat_dir);
    snprintf(chat2, sizeof(chat2), "%s/live2.chat", chat_dir);
    touch(chat1);
    touch(chat2);

    /* 1. seed: creates correct entries */
    {
        int rc = registry_seed(nbs_root, registry_path);
        CHECK("seed returns 0", rc == 0);

        int lines = count_file_lines(registry_path);
        CHECK("seed creates 3 entries", lines == 3);

        /* L3: registry entries */
        char entry1[L3], entry2[L3], entry3[L3];
        snprintf(entry1, sizeof(entry1), "chat:%s", chat1);
        snprintf(entry2, sizeof(entry2), "chat:%s", chat2);
        snprintf(entry3, sizeof(entry3), "bus:%s", events_dir);

        CHECK("seed contains chat1",
              file_contains_line(registry_path, entry1));
        CHECK("seed contains chat2",
              file_contains_line(registry_path, entry2));
        CHECK("seed contains bus",
              file_contains_line(registry_path, entry3));
    }

    /* 2. seed idempotent: calling twice does not create duplicates */
    {
        int rc = registry_seed(nbs_root, registry_path);
        CHECK("seed idempotent returns 0", rc == 0);

        int lines = count_file_lines(registry_path);
        CHECK("seed idempotent still 3 entries", lines == 3);
    }

    /* 3. find_first chat: returns path of first chat entry */
    {
        char out[L2] = {0};
        int rc = registry_find_first(registry_path, "chat", out, sizeof(out));
        CHECK("find_first chat returns 0", rc == 0);
        CHECK("find_first chat path is non-empty", strlen(out) > 0);
        CHECK("find_first chat path ends with .chat",
              strcmp(out + strlen(out) - 5, ".chat") == 0);
    }

    /* 4. find_first bus: returns path of bus entry */
    {
        char out[L2] = {0};
        int rc = registry_find_first(registry_path, "bus", out, sizeof(out));
        CHECK("find_first bus returns 0", rc == 0);
        CHECK("find_first bus path matches events_dir",
              strcmp(out, events_dir) == 0);
    }

    /* 5. find_first nonexistent type: returns -1 */
    {
        char out[L2] = {0};
        int rc = registry_find_first(registry_path, "nonexistent",
                                     out, sizeof(out));
        CHECK("find_first nonexistent returns -1", rc == -1);
    }

    /* 6. for_each: counts entries correctly */
    {
        int chat_count = 0;
        int rc = registry_for_each(registry_path, "chat",
                                   count_callback, &chat_count);
        CHECK("for_each chat returns 2", rc == 2);
        CHECK("for_each chat callback count is 2", chat_count == 2);

        int bus_count = 0;
        rc = registry_for_each(registry_path, "bus",
                               count_callback, &bus_count);
        CHECK("for_each bus returns 1", rc == 1);
        CHECK("for_each bus callback count is 1", bus_count == 1);
    }

    /* 7. process_inbox register-chat: adds new entry */
    {
        write_file(inbox_path, "register-chat /tmp/new.chat\n");

        int inbox_line = 0;
        int rc = registry_process_inbox(inbox_path, registry_path,
                                        &inbox_line);
        CHECK("process_inbox register-chat returns 1", rc == 1);
        CHECK("process_inbox register-chat inbox_line updated",
              inbox_line == 1);
        CHECK("process_inbox register-chat entry added",
              file_contains_line(registry_path, "chat:/tmp/new.chat"));

        int lines = count_file_lines(registry_path);
        CHECK("process_inbox register-chat total entries is 4", lines == 4);
    }

    /* 8. process_inbox unregister-chat: removes entry */
    {
        write_file(inbox_path, "unregister-chat /tmp/new.chat\n");

        int inbox_line = 0;
        int rc = registry_process_inbox(inbox_path, registry_path,
                                        &inbox_line);
        CHECK("process_inbox unregister-chat returns 1", rc == 1);
        CHECK("process_inbox unregister-chat entry removed",
              !file_contains_line(registry_path, "chat:/tmp/new.chat"));

        int lines = count_file_lines(registry_path);
        CHECK("process_inbox unregister-chat total entries is 3", lines == 3);
    }

    /* 9. process_inbox forward-only: set inbox_line=2, only processes new */
    {
        write_file(inbox_path,
                   "register-chat /tmp/old1.chat\n"
                   "register-chat /tmp/old2.chat\n"
                   "register-chat /tmp/new1.chat\n"
                   "register-chat /tmp/new2.chat\n"
                   "register-chat /tmp/new3.chat\n");

        int inbox_line = 2;
        int rc = registry_process_inbox(inbox_path, registry_path,
                                        &inbox_line);
        CHECK("process_inbox forward-only returns 3", rc == 3);
        CHECK("process_inbox forward-only inbox_line is 5", inbox_line == 5);

        CHECK("process_inbox forward-only old1 not registered",
              !file_contains_line(registry_path, "chat:/tmp/old1.chat"));
        CHECK("process_inbox forward-only old2 not registered",
              !file_contains_line(registry_path, "chat:/tmp/old2.chat"));

        CHECK("process_inbox forward-only new1 registered",
              file_contains_line(registry_path, "chat:/tmp/new1.chat"));
        CHECK("process_inbox forward-only new2 registered",
              file_contains_line(registry_path, "chat:/tmp/new2.chat"));
        CHECK("process_inbox forward-only new3 registered",
              file_contains_line(registry_path, "chat:/tmp/new3.chat"));
    }

    /* 10. process_inbox comments skipped */
    {
        /* Clean up entries from test 9 */
        write_file(inbox_path, "unregister-chat /tmp/new1.chat\n"
                               "unregister-chat /tmp/new2.chat\n"
                               "unregister-chat /tmp/new3.chat\n");
        int cleanup_line = 0;
        registry_process_inbox(inbox_path, registry_path, &cleanup_line);

        int lines_before = count_file_lines(registry_path);

        write_file(inbox_path,
                   "# this is a comment\n"
                   "register-chat /tmp/comment_test.chat\n"
                   "# another comment\n");

        int inbox_line = 0;
        int rc = registry_process_inbox(inbox_path, registry_path,
                                        &inbox_line);
        CHECK("process_inbox comments: returns 1 (only real command)", rc == 1);
        CHECK("process_inbox comments: entry added",
              file_contains_line(registry_path, "chat:/tmp/comment_test.chat"));

        int lines_after = count_file_lines(registry_path);
        CHECK("process_inbox comments: exactly 1 new entry",
              lines_after == lines_before + 1);

        /* Clean up */
        write_file(inbox_path, "unregister-chat /tmp/comment_test.chat\n");
        cleanup_line = 0;
        registry_process_inbox(inbox_path, registry_path, &cleanup_line);
    }

    /* 11. process_inbox empty lines skipped */
    {
        int lines_before = count_file_lines(registry_path);

        write_file(inbox_path,
                   "\n"
                   "\n"
                   "register-chat /tmp/empty_test.chat\n"
                   "\n");

        int inbox_line = 0;
        int rc = registry_process_inbox(inbox_path, registry_path,
                                        &inbox_line);
        CHECK("process_inbox empty lines: returns 1", rc == 1);
        CHECK("process_inbox empty lines: entry added",
              file_contains_line(registry_path, "chat:/tmp/empty_test.chat"));

        int lines_after = count_file_lines(registry_path);
        CHECK("process_inbox empty lines: exactly 1 new entry",
              lines_after == lines_before + 1);
    }

    /* Clean up */
    rmrf(tmpdir);

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
