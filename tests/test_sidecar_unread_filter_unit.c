/*
 * test_sidecar_unread_filter_unit.c — Unit tests for filtering own messages
 * out of the unread count.
 *
 * Falsifiable claims tested:
 *
 *   1. Empty chat: count_unread_others returns 0 regardless of cursor
 *   2. Chat with only own messages: returns 0
 *   3. Chat with only others' messages: returns count of all past cursor
 *   4. Mixed chat: returns count of others past cursor only
 *   5. Cursor at end: returns 0
 *   6. Cursor past end: returns 0 (clamped behaviour)
 *   7. Cursor of -1 (no entry): treats all messages as past cursor
 *   8. Returns -1 if chat file does not exist
 *
 * The fix being tested: sidecar's check_unread_cb should not count
 * the agent's own messages as unread. Previously the count was
 * total - cursor - 1 (no sender filter), causing agents to be
 * notified about their own posts.
 */

#include "../src/nbs-sidecar/chat_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

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

static char g_test_dir[256];
static char g_chat_path[512];

/* Run nbs-chat to create or send to a test chat file */
static int run_nbs_chat(const char *cmd, const char *handle, const char *text) {
    pid_t pid = fork();
    if (pid == 0) {
        if (text) {
            execlp("nbs-chat", "nbs-chat", cmd, g_chat_path, handle, text, (char *)NULL);
        } else if (handle) {
            execlp("nbs-chat", "nbs-chat", cmd, g_chat_path, handle, (char *)NULL);
        } else {
            execlp("nbs-chat", "nbs-chat", cmd, g_chat_path, (char *)NULL);
        }
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WEXITSTATUS(status);
}

static void setup(const char *test_name) {
    snprintf(g_test_dir, sizeof(g_test_dir), "/tmp/nbs_test_unread_%s_%d",
             test_name, getpid());
    mkdir(g_test_dir, 0700);
    snprintf(g_chat_path, sizeof(g_chat_path), "%s/test.chat", g_test_dir);
    run_nbs_chat("create", NULL, NULL);
}

static void teardown(void) {
    char rm_cmd[512];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", g_test_dir);
    int rc = system(rm_cmd);
    (void)rc;
}

int main(void) {
    printf("test_sidecar_unread_filter_unit\n");

    /* 1. Empty chat */
    {
        setup("empty");
        int unread = chat_client_count_unread_others(g_chat_path, "alice", -1);
        CHECK("empty chat returns 0", unread == 0);
        teardown();
    }

    /* 2. Only own messages */
    {
        setup("own_only");
        run_nbs_chat("send", "alice", "hello from alice");
        run_nbs_chat("send", "alice", "another from alice");
        int unread = chat_client_count_unread_others(g_chat_path, "alice", -1);
        CHECK("own messages only = 0 unread", unread == 0);
        teardown();
    }

    /* 3. Only others' messages */
    {
        setup("others_only");
        run_nbs_chat("send", "bob", "from bob");
        run_nbs_chat("send", "carol", "from carol");
        int unread = chat_client_count_unread_others(g_chat_path, "alice", -1);
        CHECK("others' messages only = 2 unread", unread == 2);
        teardown();
    }

    /* 4. Mixed: should count only others past cursor */
    {
        setup("mixed");
        run_nbs_chat("send", "bob", "msg 0 from bob");
        run_nbs_chat("send", "alice", "msg 1 from alice");
        run_nbs_chat("send", "alice", "msg 2 from alice");
        run_nbs_chat("send", "bob", "msg 3 from bob");
        run_nbs_chat("send", "carol", "msg 4 from carol");
        /* From cursor=-1: bob, alice, alice, bob, carol → 3 others (bob, bob, carol) */
        int unread = chat_client_count_unread_others(g_chat_path, "alice", -1);
        CHECK("mixed from start: 3 others", unread == 3);

        /* From cursor=2 (msg 2 is alice's): msg 3 bob, msg 4 carol → 2 others */
        unread = chat_client_count_unread_others(g_chat_path, "alice", 2);
        CHECK("mixed from cursor=2: 2 others", unread == 2);
        teardown();
    }

    /* 5. Cursor at end */
    {
        setup("cursor_end");
        run_nbs_chat("send", "bob", "from bob");
        run_nbs_chat("send", "carol", "from carol");
        /* cursor=1 means last-read index is 1 (the second msg, 0-indexed) */
        int unread = chat_client_count_unread_others(g_chat_path, "alice", 1);
        CHECK("cursor at end = 0 unread", unread == 0);
        teardown();
    }

    /* 6. Cursor past end (post-archive scenario) */
    {
        setup("cursor_past");
        run_nbs_chat("send", "bob", "from bob");
        int unread = chat_client_count_unread_others(g_chat_path, "alice", 999);
        CHECK("cursor past end = 0 unread", unread == 0);
        teardown();
    }

    /* 7. Missing chat file */
    {
        snprintf(g_chat_path, sizeof(g_chat_path), "/tmp/nonexistent_chat_%d.chat", getpid());
        int unread = chat_client_count_unread_others(g_chat_path, "alice", -1);
        CHECK("missing file returns -1", unread == -1);
    }

    printf("\n%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
