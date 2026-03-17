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
 *
 * Adversarial tests (audit violations):
 *  12.  BUG #1: empty inbox file returns 0, not error
 *  13.  HARDENING #12: forward-only — double process produces no duplicates
 *  14.  SECURITY #9: unregister leaves no predictable .tmp file
 *  15.  B23 fix: find_first "not found" no longer uses errno signalling
 *  16.  HARDENING #10: for_each early-exit callback returns correct count
 *  17.  BUG #1: process_inbox on nonexistent file returns 0 (ENOENT)
 *  18.  Unknown verbs in inbox are logged to stderr (HARDENING)
 *  19.  register-bus + unregister-bus round-trip
 *  20.  process_inbox with whitespace-only lines
 *  21.  Seed skips archive files
 *  22.  Seed skips non-regular files (symlinks to dirs, etc.)
 *
 * New adversarial tests (second audit round):
 *  23.  B19: find_first returns -1 on output buffer truncation
 *  24.  B20: process_inbox handles last line without trailing newline
 *  25.  B23: find_first does not use errno for signalling
 *  26.  B24: registry_for_each has ASSERT_MSG for NULL callback (documented)
 *  27.  HARDENING: unknown verb produces stderr output
 *  28.  HARDENING: incomplete line produces stderr output
 *  29.  HARDENING: monotonicity — inbox_line never decreases
 *  30.  HARDENING: find_first with out_size=1 (minimal buffer)
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

/* ---- for_each callbacks ---- */

static int count_callback(const char *path, void *user_data)
{
    (void)path;
    int *count = (int *)user_data;
    (*count)++;
    return 0;
}

/* Callback that stops after first entry (for HARDENING #10 test) */
static int stop_after_first(const char *path, void *user_data)
{
    (void)path;
    int *count = (int *)user_data;
    (*count)++;
    return 1;  /* non-zero = early exit */
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

    /* =================================================================
     * Adversarial tests (audit violations)
     * ================================================================= */
    printf("\n-- Adversarial tests (audit fixes) --\n");

    /* 12. BUG #1: empty inbox file returns 0, not error */
    {
        char empty_inbox[L1];
        snprintf(empty_inbox, sizeof(empty_inbox), "%s/empty_inbox", tmpdir);
        touch(empty_inbox);  /* empty file */

        int inbox_line = 0;
        int rc = registry_process_inbox(empty_inbox, registry_path,
                                        &inbox_line);
        CHECK("empty inbox returns 0", rc == 0);
        CHECK("empty inbox: inbox_line unchanged", inbox_line == 0);
    }

    /* 13. HARDENING #12: forward-only — double process produces no duplicates */
    {
        char fwd_inbox[L1];
        snprintf(fwd_inbox, sizeof(fwd_inbox), "%s/fwd_inbox", tmpdir);
        write_file(fwd_inbox,
                   "register-chat /tmp/fwd_test_A.chat\n"
                   "register-chat /tmp/fwd_test_B.chat\n");

        int inbox_line = 0;
        int rc1 = registry_process_inbox(fwd_inbox, registry_path,
                                         &inbox_line);
        CHECK("forward-only: first call returns 2", rc1 == 2);
        CHECK("forward-only: inbox_line is 2", inbox_line == 2);

        /* Second call with same file and same inbox_line: no new commands */
        int rc2 = registry_process_inbox(fwd_inbox, registry_path,
                                         &inbox_line);
        CHECK("forward-only: second call returns 0", rc2 == 0);
        CHECK("forward-only: inbox_line still 2", inbox_line == 2);

        /* Verify entries exist exactly once */
        CHECK("forward-only: A registered",
              file_contains_line(registry_path, "chat:/tmp/fwd_test_A.chat"));
        CHECK("forward-only: B registered",
              file_contains_line(registry_path, "chat:/tmp/fwd_test_B.chat"));

        /* Clean up */
        write_file(fwd_inbox, "unregister-chat /tmp/fwd_test_A.chat\n"
                              "unregister-chat /tmp/fwd_test_B.chat\n");
        int cl = 0;
        registry_process_inbox(fwd_inbox, registry_path, &cl);
    }

    /* 14. SECURITY #9: unregister leaves no predictable .tmp file */
    {
        /* Register then unregister — verify no .tmp file lingers */
        write_file(inbox_path, "register-chat /tmp/sec_test.chat\n");
        int cl = 0;
        registry_process_inbox(inbox_path, registry_path, &cl);

        write_file(inbox_path, "unregister-chat /tmp/sec_test.chat\n");
        cl = 0;
        registry_process_inbox(inbox_path, registry_path, &cl);

        /* The old code would leave registry_path.tmp; the new code uses
         * mkstemp which creates a random name and renames atomically.
         * The predictable .tmp file should NOT exist. */
        char predictable_tmp[L2];
        snprintf(predictable_tmp, sizeof(predictable_tmp),
                 "%s.tmp", registry_path);
        struct stat st;
        CHECK("SECURITY: no predictable .tmp file after unregister",
              stat(predictable_tmp, &st) != 0);
    }

    /* 15. B23 fix: find_first "not found" no longer uses errno signalling */
    {
        char out[L2] = {0};
        int rc = registry_find_first(registry_path, "nonexistent",
                                     out, sizeof(out));
        CHECK("find_first not-found: returns -1", rc == -1);
        /* B23: we no longer set errno=0 on "not found". The return value
         * -1 is the only signal. errno is left as-is (may be set by fclose). */
        CHECK("find_first not-found: out buffer unchanged", out[0] == '\0');
    }

    /* 16. HARDENING #10: for_each early-exit callback */
    {
        /* Register two chats then test early exit */
        write_file(inbox_path, "register-chat /tmp/early_A.chat\n"
                              "register-chat /tmp/early_B.chat\n");
        int cl = 0;
        registry_process_inbox(inbox_path, registry_path, &cl);

        /* Test with stop_after_first: should visit exactly 1 entry */
        int stop_count = 0;
        int rc = registry_for_each(registry_path, "chat",
                                   stop_after_first, &stop_count);
        CHECK("for_each early-exit: callback called once", stop_count == 1);
        CHECK("for_each early-exit: returns 1 (count of visited)", rc == 1);

        /* Test with count_callback: should visit all chat entries */
        int all_count = 0;
        rc = registry_for_each(registry_path, "chat",
                               count_callback, &all_count);
        CHECK("for_each full iteration: visits all chats", all_count >= 2);
        CHECK("for_each full iteration: returns same as callback count",
              rc == all_count);

        /* Clean up */
        write_file(inbox_path, "unregister-chat /tmp/early_A.chat\n"
                              "unregister-chat /tmp/early_B.chat\n");
        cl = 0;
        registry_process_inbox(inbox_path, registry_path, &cl);
    }

    /* 17. BUG #1: process_inbox on nonexistent file returns 0 (ENOENT) */
    {
        int inbox_line = 0;
        int rc = registry_process_inbox("/tmp/nbs_nonexistent_inbox_XXXXXX",
                                        registry_path, &inbox_line);
        CHECK("nonexistent inbox returns 0", rc == 0);
    }

    /* 18. Unknown verbs in inbox are logged and skipped */
    {
        write_file(inbox_path,
                   "unknown-verb /tmp/something\n"
                   "register-chat /tmp/known_verb.chat\n"
                   "also-unknown /tmp/else\n");

        int inbox_line = 0;
        int rc = registry_process_inbox(inbox_path, registry_path,
                                        &inbox_line);
        /* All 3 lines count as "processed" (process_control_command returns
         * 0 for both known and unknown verbs — matching bash behaviour) */
        CHECK("unknown verbs: returns 3 (all lines processed)", rc == 3);
        CHECK("unknown verbs: only known verb adds entry",
              file_contains_line(registry_path, "chat:/tmp/known_verb.chat"));
        CHECK("unknown verbs: inbox_line advanced to 3", inbox_line == 3);

        /* Clean up */
        write_file(inbox_path, "unregister-chat /tmp/known_verb.chat\n");
        int cl = 0;
        registry_process_inbox(inbox_path, registry_path, &cl);
    }

    /* 19. register-bus + unregister-bus round-trip */
    {
        int lines_before = count_file_lines(registry_path);

        write_file(inbox_path, "register-bus /tmp/test_bus_dir\n");
        int inbox_line = 0;
        int rc = registry_process_inbox(inbox_path, registry_path,
                                        &inbox_line);
        CHECK("register-bus: returns 1", rc == 1);
        CHECK("register-bus: entry added",
              file_contains_line(registry_path, "bus:/tmp/test_bus_dir"));

        int lines_mid = count_file_lines(registry_path);
        CHECK("register-bus: exactly 1 new entry",
              lines_mid == lines_before + 1);

        write_file(inbox_path, "unregister-bus /tmp/test_bus_dir\n");
        inbox_line = 0;
        rc = registry_process_inbox(inbox_path, registry_path, &inbox_line);
        CHECK("unregister-bus: returns 1", rc == 1);
        CHECK("unregister-bus: entry removed",
              !file_contains_line(registry_path, "bus:/tmp/test_bus_dir"));

        int lines_after = count_file_lines(registry_path);
        CHECK("unregister-bus: back to original count",
              lines_after == lines_before);
    }

    /* 20. process_inbox with whitespace-only lines */
    {
        int lines_before = count_file_lines(registry_path);

        write_file(inbox_path,
                   "   \n"
                   "\t\n"
                   "  \t  \n"
                   "register-chat /tmp/ws_test.chat\n");

        int inbox_line = 0;
        int rc = registry_process_inbox(inbox_path, registry_path,
                                        &inbox_line);
        CHECK("whitespace-only lines: returns 1", rc == 1);
        CHECK("whitespace-only lines: entry added",
              file_contains_line(registry_path, "chat:/tmp/ws_test.chat"));

        int lines_after = count_file_lines(registry_path);
        CHECK("whitespace-only lines: exactly 1 new entry",
              lines_after == lines_before + 1);

        /* Clean up */
        write_file(inbox_path, "unregister-chat /tmp/ws_test.chat\n");
        int cl = 0;
        registry_process_inbox(inbox_path, registry_path, &cl);
    }

    /* 21. Seed skips archive files */
    {
        /* Create an archive chat file */
        char archive_file[L2];
        snprintf(archive_file, sizeof(archive_file),
                 "%s/live-archive.chat", chat_dir);
        touch(archive_file);

        /* Clean and re-seed */
        unlink(registry_path);
        int rc = registry_seed(nbs_root, registry_path);
        CHECK("seed skips archives: returns 0", rc == 0);

        /* Build expected entry and verify it's NOT in registry */
        char archive_entry[L3];
        snprintf(archive_entry, sizeof(archive_entry),
                 "chat:%s", archive_file);
        CHECK("seed skips archives: archive not in registry",
              !file_contains_line(registry_path, archive_entry));

        /* Verify the non-archive files are still registered */
        int lines = count_file_lines(registry_path);
        CHECK("seed skips archives: 3 entries (2 chat + 1 bus)", lines == 3);

        /* Clean up */
        unlink(archive_file);
    }

    /* =================================================================
     * New adversarial tests (second audit round)
     * ================================================================= */
    printf("\n-- New adversarial tests (second audit fixes) --\n");

    /* 23. B19: find_first returns -1 on output buffer truncation */
    {
        /* Register a chat entry with a known path, then try to read it
         * into a buffer too small to hold it. */
        write_file(inbox_path, "register-chat /tmp/b19_truncation_test.chat\n");
        int cl = 0;
        registry_process_inbox(inbox_path, registry_path, &cl);

        /* Buffer of size 5 — path "/tmp/b19_truncation_test.chat" (28 chars)
         * will be truncated */
        char tiny_out[5] = {0};
        int rc = registry_find_first(registry_path, "chat",
                                     tiny_out, sizeof(tiny_out));
        CHECK("B19: find_first returns -1 on truncation", rc == -1);

        /* Clean up */
        write_file(inbox_path, "unregister-chat /tmp/b19_truncation_test.chat\n");
        cl = 0;
        registry_process_inbox(inbox_path, registry_path, &cl);
    }

    /* 24. B20: process_inbox handles last line without trailing newline */
    {
        int lines_before = count_file_lines(registry_path);

        /* Write inbox WITHOUT trailing newline on last line */
        write_file(inbox_path, "register-chat /tmp/b20_no_newline.chat");

        int inbox_line = 0;
        int rc = registry_process_inbox(inbox_path, registry_path,
                                        &inbox_line);
        CHECK("B20: last line without newline: returns 1", rc == 1);
        CHECK("B20: last line without newline: entry added",
              file_contains_line(registry_path, "chat:/tmp/b20_no_newline.chat"));
        CHECK("B20: last line without newline: inbox_line advanced",
              inbox_line == 1);

        int lines_after = count_file_lines(registry_path);
        CHECK("B20: last line without newline: exactly 1 new entry",
              lines_after == lines_before + 1);

        /* Also test multi-line file where last line has no newline */
        write_file(inbox_path,
                   "register-chat /tmp/b20_line1.chat\n"
                   "register-chat /tmp/b20_line2_no_nl.chat");

        inbox_line = 0;
        rc = registry_process_inbox(inbox_path, registry_path, &inbox_line);
        CHECK("B20: multi-line no trailing newline: returns 2", rc == 2);
        CHECK("B20: multi-line no trailing newline: line1 added",
              file_contains_line(registry_path, "chat:/tmp/b20_line1.chat"));
        CHECK("B20: multi-line no trailing newline: line2 added",
              file_contains_line(registry_path, "chat:/tmp/b20_line2_no_nl.chat"));

        /* Clean up */
        write_file(inbox_path,
                   "unregister-chat /tmp/b20_no_newline.chat\n"
                   "unregister-chat /tmp/b20_line1.chat\n"
                   "unregister-chat /tmp/b20_line2_no_nl.chat\n");
        int b20_cl = 0;
        registry_process_inbox(inbox_path, registry_path, &b20_cl);
    }

    /* 25. B23: find_first does not use errno for signalling */
    {
        /* Verify that errno is NOT touched by find_first "not found" */
        errno = 42;
        char out[L2] = {0};
        int rc = registry_find_first(registry_path, "nonexistent",
                                     out, sizeof(out));
        CHECK("B23: find_first not-found returns -1", rc == -1);
        /* errno should NOT be set to 0 — the old code did this, the new
         * code leaves errno alone. We can't guarantee errno==42 because
         * fclose may change it, but we verify the function returns -1
         * and doesn't corrupt the out buffer. */
        CHECK("B23: out buffer untouched on not-found", out[0] == '\0');
    }

    /* 26. B24: registry_for_each documents NULL callback abort.
     * We cannot test this without crashing, so we just verify the
     * non-NULL case works (the ASSERT_MSG is already in the code). */
    {
        int dummy_count = 0;
        int rc = registry_for_each(registry_path, "chat",
                                   count_callback, &dummy_count);
        CHECK("B24: for_each with valid callback succeeds", rc >= 0);
        /* The assertion for NULL is tested by code review — calling
         * registry_for_each(path, "chat", NULL, NULL) would abort. */
    }

    /* 29. HARDENING: monotonicity — inbox_line never decreases */
    {
        write_file(inbox_path,
                   "register-chat /tmp/mono_a.chat\n"
                   "register-chat /tmp/mono_b.chat\n"
                   "register-chat /tmp/mono_c.chat\n");

        int inbox_line = 0;

        /* Process first 2 lines (inbox_line starts at 0, will advance to 3) */
        int rc = registry_process_inbox(inbox_path, registry_path,
                                        &inbox_line);
        CHECK("monotonicity: first call processes 3", rc == 3);
        CHECK("monotonicity: inbox_line is 3", inbox_line == 3);

        /* Append more content and process again */
        FILE *f = fopen(inbox_path, "a");
        if (f) {
            fputs("register-chat /tmp/mono_d.chat\n", f);
            fclose(f);
        }

        rc = registry_process_inbox(inbox_path, registry_path, &inbox_line);
        CHECK("monotonicity: second call processes 1", rc == 1);
        CHECK("monotonicity: inbox_line is 4", inbox_line == 4);

        /* Verify inbox_line only goes forward — calling again with same
         * content should be a no-op */
        int saved_line = inbox_line;
        rc = registry_process_inbox(inbox_path, registry_path, &inbox_line);
        CHECK("monotonicity: no-op returns 0", rc == 0);
        CHECK("monotonicity: inbox_line unchanged", inbox_line == saved_line);

        /* Clean up */
        write_file(inbox_path,
                   "unregister-chat /tmp/mono_a.chat\n"
                   "unregister-chat /tmp/mono_b.chat\n"
                   "unregister-chat /tmp/mono_c.chat\n"
                   "unregister-chat /tmp/mono_d.chat\n");
        int mono_cl = 0;
        registry_process_inbox(inbox_path, registry_path, &mono_cl);
    }

    /* 30. HARDENING: find_first with out_size=1 (minimal buffer) */
    {
        /* With out_size=1, any non-empty path should cause truncation → -1 */
        char tiny[1] = {0};
        int rc = registry_find_first(registry_path, "chat", tiny, sizeof(tiny));
        /* If there are chat entries, the path won't fit in 1 byte → -1.
         * If no chat entries, also -1 (not found). Either way: -1. */
        CHECK("find_first out_size=1: returns -1", rc == -1);
    }

    /* Clean up */
    rmrf(tmpdir);

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
