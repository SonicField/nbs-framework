/*
 * test_sidecar_chat_client_unit.c — Unit tests for chat_client module.
 *
 * Falsifiable claims tested:
 *   1.  count_messages: empty chat (header only) → 0
 *   2.  count_messages: chat with 3 messages → 3
 *   3.  count_messages: blank lines after --- not counted → correct count
 *   4.  count_messages: nonexistent file → -1
 *   5.  read_cursor: cursor exists → returns value
 *   6.  read_cursor: handle not present → -1
 *   7.  read_cursor: no cursor file → -1
 *   8.  read_cursor: comment lines skipped → correct value
 *   9.  read_cursor: cursor value 0 → returns 0
 *  10.  check_unread: all caught up → unread_count=0, returns 1
 *  11.  check_unread: has unread → correct count, returns 0, summary non-empty
 *  12.  check_unread: no chats registered → returns 2
 *  13.  are_unread_sidecar_only: all from sidecar → 1
 *  14.  are_unread_sidecar_only: mixed handles → 0
 *  15.  are_unread_sidecar_only: timestamped format → handle extracted, returns 1
 *  16.  send: successful send → returns 0
 *  17.  count_messages: long lines (>4095 chars) not double-counted
 *  18.  are_unread_sidecar_only: non-sidecar in long line → returns 0
 *
 * Adversarial tests (BUG/SECURITY violations):
 *  19.  read_cursor: INT_MAX cursor clamped to INT_MAX-1 (overflow prevention)
 *  20.  read_cursor: negative cursor clamped to 0
 *  21.  check_unread: large cursor does not cause overflow in unread count
 *  22.  check_unread: postconditions hold on return 0 (summary non-empty)
 *  23.  read_cursor: chat_path emptiness precondition (via assertion)
 *  24.  check_unread: empty chat (total=0) no spurious unreads (B15 fix)
 *  25.  are_unread_sidecar_only: unparseable message treated as non-sidecar
 *  26.  count_messages postcondition: returns >= 0 for valid file
 *  27.  read_cursor postcondition: return in [-1, INT_MAX-1]
 *  28.  check_unread: multi-chat accumulation without overflow (B14 fix)
 */

#include "../src/nbs-sidecar/chat_client.h"
#include "../src/nbs-chat/base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/*
 * Buffer tiers (satisfies -Werror=format-truncation):
 *   L0 (256)  — tmpdir base
 *   L1 (512)  — subdirectory paths
 *   L2 (768)  — file paths (chat files)
 *   L3 (1024) — cursor paths (L2 + ".cursors"), registry entries, summaries
 */
#define L0 256
#define L1 512
#define L2 768
#define L3 1024

/* Base64 encoding buffer — generous for test messages */
#define B64_BUF 4096

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

/*
 * encode_msg — Base64-encode a raw message string.
 *
 * Writes NUL-terminated base64 into out. Returns length or -1 on error.
 */
static int encode_msg(const char *raw, char *out, size_t out_size)
{
    size_t raw_len = strlen(raw);
    return base64_encode((const unsigned char *)raw, raw_len, out, out_size);
}

/*
 * write_chat_file — Create a chat file with the given base64 message lines.
 *
 * msgs is a NULL-terminated array of already-encoded base64 strings.
 * If msgs is NULL, an empty chat (header only) is written.
 */
static void write_chat_file(const char *path, const char **msgs)
{
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f,
        "=== nbs-chat ===\n"
        "last-writer: system\n"
        "last-write: 2026-02-22T00:00:00+0000\n"
        "file-length: 0\n"
        "participants: \n"
        "---\n");

    if (msgs) {
        for (int i = 0; msgs[i] != NULL; i++) {
            fprintf(f, "%s\n", msgs[i]);
        }
    }

    fclose(f);
}

/*
 * wait_ms — Portable millisecond sleep using usleep(3).
 *
 * usleep requires _DEFAULT_SOURCE or _BSD_SOURCE on glibc.
 * We pass _DEFAULT_SOURCE via the compiler flags (-D).
 */
static void wait_ms(int ms)
{
    usleep((unsigned int)(ms * 1000));
}

/* ---- Tests ---- */

int main(void)
{
    printf("test_sidecar_chat_client_unit\n");

    /* Create temp directory */
    char tmpdir[] = "/tmp/nbs_chatcli_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        fprintf(stderr, "Failed to create temp dir: %s\n", strerror(errno));
        return 1;
    }

    /* Pre-encode some test messages */
    char b64_msg1[B64_BUF], b64_msg2[B64_BUF], b64_msg3[B64_BUF];
    char b64_sc1[B64_BUF], b64_sc2[B64_BUF], b64_sc3[B64_BUF];
    char b64_claude[B64_BUF];
    char b64_ts_sc[B64_BUF];

    /* Messages with legacy format (handle: content) */
    encode_msg("alice: hello world", b64_msg1, sizeof(b64_msg1));
    encode_msg("bob: hi there", b64_msg2, sizeof(b64_msg2));
    encode_msg("alice: goodbye", b64_msg3, sizeof(b64_msg3));

    /* Messages from sidecar (legacy format) */
    encode_msg("sidecar: check-in message 1", b64_sc1, sizeof(b64_sc1));
    encode_msg("sidecar: check-in message 2", b64_sc2, sizeof(b64_sc2));
    encode_msg("sidecar: check-in message 3", b64_sc3, sizeof(b64_sc3));

    /* Message from claude */
    encode_msg("claude: I am here", b64_claude, sizeof(b64_claude));

    /* Timestamped sidecar message */
    encode_msg("sidecar|1234567890: check-in message", b64_ts_sc,
               sizeof(b64_ts_sc));

    /* ============================================================
     * 1. count_messages: empty chat (header only) → 0
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/empty.chat", tmpdir);
        write_chat_file(chat_path, NULL);

        int count = chat_client_count_messages(chat_path);
        CHECK("count_messages: empty chat returns 0", count == 0);
    }

    /* ============================================================
     * 2. count_messages: chat with 3 messages → 3
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/three.chat", tmpdir);

        const char *msgs[] = { b64_msg1, b64_msg2, b64_msg3, NULL };
        write_chat_file(chat_path, msgs);

        int count = chat_client_count_messages(chat_path);
        CHECK("count_messages: 3 messages returns 3", count == 3);
    }

    /* ============================================================
     * 3. count_messages: blank lines after --- not counted
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/blanks.chat", tmpdir);

        /* Write manually to intersperse blank lines */
        FILE *f = fopen(chat_path, "w");
        if (f) {
            fprintf(f,
                "=== nbs-chat ===\n"
                "last-writer: system\n"
                "last-write: 2026-02-22T00:00:00+0000\n"
                "file-length: 0\n"
                "participants: \n"
                "---\n"
                "%s\n"
                "\n"
                "%s\n"
                "\n"
                "\n",
                b64_msg1, b64_msg2);
            fclose(f);
        }

        int count = chat_client_count_messages(chat_path);
        CHECK("count_messages: blanks not counted, returns 2", count == 2);
    }

    /* ============================================================
     * 4. count_messages: nonexistent file → -1
     * ============================================================ */
    {
        int count = chat_client_count_messages(
            "/tmp/nbs_no_such_file_ever.chat");
        CHECK("count_messages: nonexistent file returns -1", count == -1);
    }

    /* ============================================================
     * 5. read_cursor: cursor exists → returns value
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/cursor5.chat", tmpdir);
        write_chat_file(chat_path, NULL);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=42\n");

        int cursor = chat_client_read_cursor(chat_path, "agent");
        CHECK("read_cursor: agent=42 returns 42", cursor == 42);
    }

    /* ============================================================
     * 6. read_cursor: handle not present → -1
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/cursor6.chat", tmpdir);
        write_chat_file(chat_path, NULL);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "other=10\n");

        int cursor = chat_client_read_cursor(chat_path, "agent");
        CHECK("read_cursor: handle missing returns -1", cursor == -1);
    }

    /* ============================================================
     * 7. read_cursor: no cursor file → -1
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/cursor7.chat", tmpdir);
        write_chat_file(chat_path, NULL);
        /* No .cursors file created */

        int cursor = chat_client_read_cursor(chat_path, "agent");
        CHECK("read_cursor: no cursor file returns -1", cursor == -1);
    }

    /* ============================================================
     * 8. read_cursor: comment lines skipped → correct value
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/cursor8.chat", tmpdir);
        write_chat_file(chat_path, NULL);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path,
            "# This is a comment\n"
            "# Another comment\n"
            "agent=17\n"
            "# trailing comment\n");

        int cursor = chat_client_read_cursor(chat_path, "agent");
        CHECK("read_cursor: comments skipped, returns 17", cursor == 17);
    }

    /* ============================================================
     * 9. read_cursor: cursor value 0 → returns 0
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/cursor9.chat", tmpdir);
        write_chat_file(chat_path, NULL);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=0\n");

        int cursor = chat_client_read_cursor(chat_path, "agent");
        CHECK("read_cursor: cursor value 0 returns 0", cursor == 0);
    }

    /* ============================================================
     * 10. check_unread: all caught up → unread_count=0, returns 1
     *
     * Setup: 3 messages, cursor at 2 (0-indexed last read).
     * total=3, cursor=2 → 3 > 2+1 is false → caught up.
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t10", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/live.chat", sub);

        const char *msgs[] = { b64_msg1, b64_msg2, b64_msg3, NULL };
        write_chat_file(chat_path, msgs);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=2\n");

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3];
        snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
        write_file(registry_path, entry);

        int unread_count = -1;
        char summary[L3] = {0};
        int rc = chat_client_check_unread(registry_path, "agent",
                                          &unread_count, summary,
                                          sizeof(summary));
        CHECK("check_unread: caught up returns 1", rc == 1);
        CHECK("check_unread: caught up unread_count=0", unread_count == 0);
    }

    /* ============================================================
     * 11. check_unread: has unread → correct count, returns 0
     *
     * Setup: 3 messages, cursor at 0 (read first message only).
     * total=3, cursor=0 → 3 > 0+1 is true → 2 unread.
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t11", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/live.chat", sub);

        const char *msgs[] = { b64_msg1, b64_msg2, b64_msg3, NULL };
        write_chat_file(chat_path, msgs);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=0\n");

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3];
        snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
        write_file(registry_path, entry);

        int unread_count = -1;
        char summary[L3] = {0};
        int rc = chat_client_check_unread(registry_path, "agent",
                                          &unread_count, summary,
                                          sizeof(summary));
        CHECK("check_unread: has unread returns 0", rc == 0);
        CHECK("check_unread: unread_count=2", unread_count == 2);
        CHECK("check_unread: summary non-empty", strlen(summary) > 0);
    }

    /* ============================================================
     * 12. check_unread: no chats registered → returns 2
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t12", tmpdir);
        mkdirs(sub);

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);
        /* Empty registry — no chat: entries */
        write_file(registry_path, "");

        int unread_count = -1;
        char summary[L3] = {0};
        int rc = chat_client_check_unread(registry_path, "agent",
                                          &unread_count, summary,
                                          sizeof(summary));
        CHECK("check_unread: no chats returns 2", rc == 2);
    }

    /* ============================================================
     * 13. are_unread_sidecar_only: all from sidecar → 1
     *
     * Setup: 3 sidecar messages, cursor at 0.
     * Unread messages 2,3 are both from "sidecar".
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t13", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/live.chat", sub);

        const char *msgs[] = { b64_sc1, b64_sc2, b64_sc3, NULL };
        write_chat_file(chat_path, msgs);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=0\n");

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3];
        snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
        write_file(registry_path, entry);

        int rc = chat_client_are_unread_sidecar_only(registry_path, "agent");
        CHECK("are_unread_sidecar_only: all sidecar returns 1", rc == 1);
    }

    /* ============================================================
     * 14. are_unread_sidecar_only: mixed handles → 0
     *
     * Setup: sidecar + claude messages, cursor at 0.
     * Unread includes "claude" message → not sidecar-only.
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t14", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/live.chat", sub);

        const char *msgs[] = { b64_sc1, b64_sc2, b64_claude, NULL };
        write_chat_file(chat_path, msgs);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=0\n");

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3];
        snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
        write_file(registry_path, entry);

        int rc = chat_client_are_unread_sidecar_only(registry_path, "agent");
        CHECK("are_unread_sidecar_only: mixed handles returns 0", rc == 0);
    }

    /* ============================================================
     * 15. are_unread_sidecar_only: timestamped format → returns 1
     *
     * Message format: "sidecar|1234567890: check-in message"
     * The handle extractor should strip the "|epoch" suffix.
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t15", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/live.chat", sub);

        const char *msgs[] = { b64_sc1, b64_ts_sc, b64_ts_sc, NULL };
        write_chat_file(chat_path, msgs);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=0\n");

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3];
        snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
        write_file(registry_path, entry);

        int rc = chat_client_are_unread_sidecar_only(registry_path, "agent");
        CHECK("are_unread_sidecar_only: timestamped sidecar returns 1",
              rc == 1);
    }

    /* ============================================================
     * 17. count_messages: long lines (>4095 chars) not double-counted
     *
     * A base64 line longer than MAX_LINE (4096) must be counted as
     * exactly one message, not split across multiple fgets reads.
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/longline.chat", tmpdir);

        /* Build a long raw message (>6000 chars raw → >8000 chars base64) */
        char long_raw[8192];
        memset(long_raw, 0, sizeof(long_raw));
        snprintf(long_raw, sizeof(long_raw), "agent|1234567890: ");
        size_t prefix_len = strlen(long_raw);
        /* Fill with 'A' to make total raw length ~6000 */
        memset(long_raw + prefix_len, 'A', 6000 - prefix_len);
        long_raw[6000] = '\0';

        /* Base64-encode the long message */
        char long_b64[16384];
        int b64_len = base64_encode((const unsigned char *)long_raw,
                                     strlen(long_raw), long_b64,
                                     sizeof(long_b64));
        CHECK("long line: base64 encode succeeds", b64_len > 0);
        CHECK("long line: base64 length > 4096",
              b64_len > 0 && (size_t)b64_len > 4096);

        /* Write chat file with 1 short + 1 long + 1 short message */
        FILE *f = fopen(chat_path, "w");
        if (f) {
            fprintf(f,
                "=== nbs-chat ===\n"
                "last-writer: system\n"
                "last-write: 2026-02-22T00:00:00+0000\n"
                "file-length: 0\n"
                "participants: \n"
                "---\n"
                "%s\n"
                "%s\n"
                "%s\n",
                b64_msg1, long_b64, b64_msg3);
            fclose(f);
        }

        int count = chat_client_count_messages(chat_path);
        CHECK("count_messages: long line not double-counted, returns 3",
              count == 3);
    }

    /* ============================================================
     * 18. are_unread_sidecar_only: non-sidecar in long line → 0
     *
     * A non-sidecar message whose base64 exceeds the old 4096-byte
     * buffer must still be decoded and identified as non-sidecar.
     * Previously, long lines were skipped (undecodable), causing
     * the function to incorrectly return 1 (sidecar-only).
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t18", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/live.chat", sub);

        /* Build a long non-sidecar message (>6000 chars raw → >8000 chars b64) */
        char long_raw[8192];
        memset(long_raw, 0, sizeof(long_raw));
        snprintf(long_raw, sizeof(long_raw), "generalist|1234567890: ");
        size_t prefix_len = strlen(long_raw);
        memset(long_raw + prefix_len, 'X', 6000 - prefix_len);
        long_raw[6000] = '\0';

        char long_b64[16384];
        int b64_len = base64_encode((const unsigned char *)long_raw,
                                     strlen(long_raw), long_b64,
                                     sizeof(long_b64));
        CHECK("t18: base64 encode succeeds", b64_len > 0);
        CHECK("t18: base64 length > 4096",
              b64_len > 0 && (size_t)b64_len > 4096);

        /* Chat: 1 sidecar msg (read) + 1 sidecar + 1 long non-sidecar (unread) */
        FILE *f = fopen(chat_path, "w");
        if (f) {
            fprintf(f,
                "=== nbs-chat ===\n"
                "last-writer: system\n"
                "last-write: 2026-02-22T00:00:00+0000\n"
                "file-length: 0\n"
                "participants: \n"
                "---\n"
                "%s\n"
                "%s\n"
                "%s\n",
                b64_sc1, b64_sc2, long_b64);
            fclose(f);
        }

        /* Cursor at 0 → messages 1,2 are unread (sidecar + generalist) */
        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=0\n");

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3];
        snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
        write_file(registry_path, entry);

        int rc = chat_client_are_unread_sidecar_only(registry_path, "agent");
        CHECK("are_unread_sidecar_only: long non-sidecar returns 0", rc == 0);
    }

    /* ============================================================
     * 16. send: successful send → returns 0
     *
     * Requires nbs-chat in PATH. Creates a chat file, sends a
     * message, then verifies the message count increased.
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t16", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/live.chat", sub);

        /* Create the chat file using nbs-chat create (via system)
         * so the file is properly formatted for nbs-chat send. */
        char cmd[L3];
        snprintf(cmd, sizeof(cmd), "nbs-chat create '%s' 2>/dev/null",
                 chat_path);
        int sys_rc = system(cmd);

        if (sys_rc != 0) {
            /* nbs-chat not in PATH — skip with warning */
            printf("   SKIP: send: nbs-chat not in PATH "
                   "(set PATH to include bin/)\n");
        } else {
            int before = chat_client_count_messages(chat_path);
            CHECK("send: initial count is 0", before == 0);

            int rc = chat_client_send(chat_path, "test-handle",
                                      "hello from unit test");
            CHECK("send: returns 0", rc == 0);

            /* Give the fire-and-forget child a moment to complete */
            wait_ms(200);

            int after = chat_client_count_messages(chat_path);
            CHECK("send: message count increased to 1", after == 1);
        }
    }

    /* ============================================================
     * 19. read_cursor: INT_MAX cursor clamped to INT_MAX-1
     *
     * BUG: cursor + 1 overflows when cursor is INT_MAX. After fix,
     * read_cursor clamps to INT_MAX - 1. The comparison
     * total - 1 > cursor is also overflow-safe.
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/cursor_overflow.chat",
                 tmpdir);
        write_chat_file(chat_path, NULL);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=2147483647\n");

        int cursor = chat_client_read_cursor(chat_path, "agent");
        CHECK("read_cursor: INT_MAX clamped to INT_MAX-1",
              cursor == 2147483646);
    }

    /* ============================================================
     * 20. read_cursor: negative cursor clamped to 0
     *
     * BUG: sscanf with %d allowed negative values. After fix,
     * negative values are clamped to 0.
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/cursor_neg.chat", tmpdir);
        write_chat_file(chat_path, NULL);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=-5\n");

        int cursor = chat_client_read_cursor(chat_path, "agent");
        CHECK("read_cursor: negative value clamped to 0", cursor == 0);
    }

    /* ============================================================
     * 21. check_unread: large cursor does not cause overflow
     *
     * BUG: total > cursor + 1 overflows when cursor is large.
     * After fix: comparison uses total - 1 > cursor (safe because
     * total >= 0) and cursor is clamped by read_cursor.
     *
     * With cursor at INT_MAX-1 and total=3, we expect:
     *   total - 1 > cursor → 2 > 2147483646 → false → caught up
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t21", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/live.chat", sub);

        const char *msgs[] = { b64_msg1, b64_msg2, b64_msg3, NULL };
        write_chat_file(chat_path, msgs);

        /* cursor = INT_MAX → clamped to INT_MAX-1 by read_cursor */
        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=2147483647\n");

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3];
        snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
        write_file(registry_path, entry);

        int unread_count = -1;
        char summary[L3] = {0};
        int rc = chat_client_check_unread(registry_path, "agent",
                                          &unread_count, summary,
                                          sizeof(summary));

        /* With cursor at 2147483646 and total=3, should be "caught up".
         * Before the fix, cursor + 1 would overflow to INT_MIN,
         * making total > INT_MIN always true → wrong unread count. */
        CHECK("overflow: returns 1 (caught up, not spurious unread)",
              rc == 1);
        CHECK("overflow: unread_count is 0", unread_count == 0);
    }

    /* ============================================================
     * 22. check_unread: postconditions on return 0
     *
     * HARDENING: when check_unread returns 0 (unread found),
     * unread_count > 0 and summary is non-empty.
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t22", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/live.chat", sub);

        const char *msgs[] = { b64_msg1, b64_msg2, b64_msg3, NULL };
        write_chat_file(chat_path, msgs);

        /* cursor at 0 → 2 unread messages */
        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=0\n");

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3];
        snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
        write_file(registry_path, entry);

        int unread_count = -1;
        char summary[L3] = {0};
        int rc = chat_client_check_unread(registry_path, "agent",
                                          &unread_count, summary,
                                          sizeof(summary));

        CHECK("postcond: returns 0 (unread)", rc == 0);
        CHECK("postcond: unread_count > 0", unread_count > 0);
        CHECK("postcond: summary non-empty", summary[0] != '\0');
        CHECK("postcond: summary contains count",
              strstr(summary, "2 unread") != NULL);
    }

    /* ============================================================
     * 24. check_unread: empty chat (total=0) does not produce
     *     spurious unreads (B15 fix).
     *
     * Before the fix, check_unread_cb had no total <= 0 guard.
     * With total=0 and cursor=0: total - 1 > cursor → -1 > 0 → false,
     * which happened to work, but total=0 with cursor=-1 (no cursor file)
     * would give: cursor clamped to 0, total-1=-1 > 0 → false.
     * The explicit guard makes the invariant falsifiable.
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t24", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/empty.chat", sub);
        write_chat_file(chat_path, NULL); /* 0 messages */
        /* No cursor file — cursor will be -1, clamped to 0 */

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3];
        snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
        write_file(registry_path, entry);

        int unread_count = -1;
        char summary[L3] = {0};
        int rc = chat_client_check_unread(registry_path, "agent",
                                          &unread_count, summary,
                                          sizeof(summary));
        CHECK("B15: empty chat returns 1 (caught up)", rc == 1);
        CHECK("B15: empty chat unread_count=0", unread_count == 0);
    }

    /* ============================================================
     * 25. are_unread_sidecar_only: unparseable message treated as
     *     non-sidecar (hardening fix).
     *
     * A message whose decoded content has no ": " delimiter cannot
     * have its handle extracted. Before the fix, such messages were
     * silently skipped — allowing false "sidecar-only" results.
     * After the fix, they are conservatively treated as non-sidecar.
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t25", tmpdir);
        mkdirs(sub);

        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/live.chat", sub);

        /* Encode a message with no ": " delimiter — unparseable */
        char b64_bad[B64_BUF];
        encode_msg("no_delimiter_here", b64_bad, sizeof(b64_bad));

        const char *msgs[] = { b64_sc1, b64_bad, NULL };
        write_chat_file(chat_path, msgs);

        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=0\n");

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3];
        snprintf(entry, sizeof(entry), "chat:%s\n", chat_path);
        write_file(registry_path, entry);

        int rc = chat_client_are_unread_sidecar_only(registry_path, "agent");
        CHECK("unparseable msg: treated as non-sidecar, returns 0", rc == 0);
    }

    /* ============================================================
     * 26. count_messages postcondition: returns >= 0 for valid file
     *
     * Verify the postcondition assertion (count >= 0) holds for
     * a chat file with known content.
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/postcond.chat", tmpdir);

        const char *msgs[] = { b64_msg1, NULL };
        write_chat_file(chat_path, msgs);

        int count = chat_client_count_messages(chat_path);
        CHECK("postcond: count_messages >= 0 for valid file", count >= 0);
        CHECK("postcond: count_messages == 1", count == 1);
    }

    /* ============================================================
     * 27. read_cursor postcondition: return in [-1, INT_MAX-1]
     *
     * Verify the postcondition assertion on read_cursor holds for
     * various cursor values.
     * ============================================================ */
    {
        char chat_path[L2];
        snprintf(chat_path, sizeof(chat_path), "%s/postcond_cursor.chat",
                 tmpdir);
        write_chat_file(chat_path, NULL);

        /* Normal case */
        char cursor_path[L3];
        snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", chat_path);
        write_file(cursor_path, "agent=100\n");

        int cursor = chat_client_read_cursor(chat_path, "agent");
        CHECK("postcond: read_cursor in range [-1, INT_MAX-1]",
              cursor >= -1 && cursor <= 2147483646);
        CHECK("postcond: read_cursor == 100", cursor == 100);

        /* Missing handle case */
        int missing = chat_client_read_cursor(chat_path, "nonexistent");
        CHECK("postcond: missing handle returns -1", missing == -1);
    }

    /* ============================================================
     * 28. check_unread: multiple chats accumulate unread correctly
     *     without overflow (B14 fix verification).
     *
     * Two chat files each with 3 messages, cursor at 0 in both.
     * Total unread = 2 + 2 = 4 (not overflowed).
     * ============================================================ */
    {
        char sub[L1];
        snprintf(sub, sizeof(sub), "%s/t28", tmpdir);
        mkdirs(sub);

        char chat1[L2], chat2[L2];
        snprintf(chat1, sizeof(chat1), "%s/chat1.chat", sub);
        snprintf(chat2, sizeof(chat2), "%s/chat2.chat", sub);

        const char *msgs[] = { b64_msg1, b64_msg2, b64_msg3, NULL };
        write_chat_file(chat1, msgs);
        write_chat_file(chat2, msgs);

        char cur1[L3], cur2[L3];
        snprintf(cur1, sizeof(cur1), "%s.cursors", chat1);
        snprintf(cur2, sizeof(cur2), "%s.cursors", chat2);
        write_file(cur1, "agent=0\n");
        write_file(cur2, "agent=0\n");

        char registry_path[L2];
        snprintf(registry_path, sizeof(registry_path), "%s/registry", sub);

        char entry[L3 * 2];
        snprintf(entry, sizeof(entry), "chat:%s\nchat:%s\n", chat1, chat2);
        write_file(registry_path, entry);

        int unread_count = -1;
        char summary[L3] = {0};
        int rc = chat_client_check_unread(registry_path, "agent",
                                          &unread_count, summary,
                                          sizeof(summary));
        CHECK("B14: multi-chat returns 0 (unread)", rc == 0);
        CHECK("B14: multi-chat unread_count=4", unread_count == 4);
        CHECK("B14: multi-chat summary contains '4 unread'",
              strstr(summary, "4 unread") != NULL);
    }

    /* Clean up */
    rmrf(tmpdir);

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
