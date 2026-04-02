/*
 * test_chatview_unit.c — Unit tests for libchatview
 *
 * Tests:
 *   1. chatview_init / chatview_free lifecycle
 *   2. chatview_init starts cursor at last message
 *   3. chatview_search_forward finds match
 *   4. chatview_search_backward finds match
 *   5. chatview_search_forward wraps correctly (returns -1 at end)
 *   6. chatview_search sets pattern and jumps to first match
 *   7. chatview_update grows msg_flags for new messages
 *   8. chatview_update clamps cursor on message count drop
 *   9. chatview_new_count tracks messages since init
 *  10. chatview_set_key_handler stores callback
 *  11. chatview_set_status formats correctly
 *  12. chatview_content_rows calculation
 *  13. msg_flags CHATVIEW_MSG_DELETED flag
 *  14. chatview_set_poll stores callback
 *  15. chatview_init with zero messages
 *
 * Build:
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -O2 \
 *       -I../src/nbs-chat -I../src/nbs-chatview \
 *       -o test_chatview_unit test_chatview_unit.c \
 *       ../src/nbs-chatview/libchatview.a
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chatview.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    tests_passed++; \
    printf("  PASS: %s\n", name); \
} while(0)

/* --- Helpers --- */

/* Build a fake chat_state_t with N messages for testing.
 * Messages are heap-allocated so chat_state_free works. */
static chat_state_t make_state(int n) {
    chat_state_t s;
    memset(&s, 0, sizeof(s));
    s.message_count = n;
    if (n > 0) {
        s.messages = calloc((size_t)n, sizeof(chat_message_t));
        for (int i = 0; i < n; i++) {
            snprintf(s.messages[i].handle, MAX_HANDLE_LEN, "user%d", i % 3);
            char buf[128];
            snprintf(buf, sizeof(buf), "Message number %d content", i);
            s.messages[i].content = strdup(buf);
            s.messages[i].content_len = strlen(s.messages[i].content);
            s.messages[i].timestamp = 1700000000 + i * 60;
        }
    }
    return s;
}

/* --- Tests --- */

static void test_init_free(void) {
    chat_state_t s = make_state(5);
    chatview_t *cv = chatview_init(&s, "test");
    TEST_ASSERT(cv != NULL, "chatview_init returned NULL");
    TEST_ASSERT(cv->state.message_count == 5,
                "message_count = %d, expected 5", cv->state.message_count);
    TEST_ASSERT(cv->msg_flags != NULL, "msg_flags is NULL");
    TEST_ASSERT(cv->msg_flags_count == 5,
                "msg_flags_count = %d, expected 5", cv->msg_flags_count);
    TEST_ASSERT(strcmp(cv->title, "test") == 0,
                "title = '%s', expected 'test'", cv->title);
    chatview_free(cv);
    TEST_PASS("init/free lifecycle");
}

static void test_init_cursor_at_end(void) {
    chat_state_t s = make_state(10);
    chatview_t *cv = chatview_init(&s, "test");
    TEST_ASSERT(cv != NULL, "chatview_init returned NULL");
    TEST_ASSERT(cv->cursor == 9,
                "cursor = %d, expected 9 (last message)", cv->cursor);
    chatview_free(cv);
    TEST_PASS("init cursor at last message");
}

static void test_search_forward(void) {
    chat_state_t s = make_state(10);
    chatview_t *cv = chatview_init(&s, "test");

    /* Set up a search pattern */
    chatview_search(cv, "number 5");
    TEST_ASSERT(cv->search_valid == 1, "search_valid should be 1");
    TEST_ASSERT(cv->cursor == 5,
                "cursor = %d, expected 5 (first match)", cv->cursor);

    /* Search forward from position 6 */
    int found = chatview_search_forward(cv, 6);
    TEST_ASSERT(found == -1,
                "found = %d, expected -1 (no more 'number 5')", found);

    chatview_free(cv);
    TEST_PASS("search_forward finds match");
}

static void test_search_backward(void) {
    chat_state_t s = make_state(10);
    chatview_t *cv = chatview_init(&s, "test");

    chatview_search(cv, "number [0-3]");
    /* Should find first match (message 0, 1, 2, or 3) */
    TEST_ASSERT(cv->search_valid == 1, "search_valid should be 1");
    TEST_ASSERT(cv->cursor <= 3,
                "cursor = %d, expected <= 3", cv->cursor);

    /* Search backward from message 9 */
    int found = chatview_search_backward(cv, 9);
    TEST_ASSERT(found == 3,
                "found = %d, expected 3 (last match for [0-3])", found);

    chatview_free(cv);
    TEST_PASS("search_backward finds match");
}

static void test_search_forward_wraps(void) {
    chat_state_t s = make_state(5);
    chatview_t *cv = chatview_init(&s, "test");

    chatview_search(cv, "number 2");
    /* Should find message 2 */
    TEST_ASSERT(cv->cursor == 2,
                "cursor = %d, expected 2", cv->cursor);

    /* Search from position 3 onwards — should not find */
    int found = chatview_search_forward(cv, 3);
    TEST_ASSERT(found == -1,
                "found = %d, expected -1 (no match after 3)", found);

    chatview_free(cv);
    TEST_PASS("search_forward returns -1 at end");
}

static void test_search_sets_pattern(void) {
    chat_state_t s = make_state(10);
    chatview_t *cv = chatview_init(&s, "test");

    chatview_search(cv, "user1");
    TEST_ASSERT(cv->search_valid == 1, "search_valid should be 1");
    TEST_ASSERT(strcmp(cv->search, "user1") == 0,
                "search = '%s', expected 'user1'", cv->search);
    /* user1 handles appear at indices 1, 4, 7 */
    TEST_ASSERT(cv->cursor == 1,
                "cursor = %d, expected 1 (first user1)", cv->cursor);

    chatview_free(cv);
    TEST_PASS("chatview_search sets pattern and jumps");
}

static void test_update_grows_flags(void) {
    chat_state_t s1 = make_state(5);
    chatview_t *cv = chatview_init(&s1, "test");

    /* Mark message 2 as deleted */
    cv->msg_flags[2] = CHATVIEW_MSG_DELETED;

    /* Update with more messages */
    chat_state_t s2 = make_state(8);
    chatview_update(cv, &s2);

    TEST_ASSERT(cv->state.message_count == 8,
                "message_count = %d, expected 8", cv->state.message_count);
    TEST_ASSERT(cv->msg_flags_count == 8,
                "msg_flags_count = %d, expected 8", cv->msg_flags_count);
    /* Old flag preserved */
    TEST_ASSERT(cv->msg_flags[2] & CHATVIEW_MSG_DELETED,
                "msg_flags[2] should still be DELETED");
    /* New flags zeroed */
    TEST_ASSERT(cv->msg_flags[5] == 0,
                "msg_flags[5] = %d, expected 0", cv->msg_flags[5]);

    chatview_free(cv);
    TEST_PASS("chatview_update grows msg_flags");
}

static void test_update_clamps_cursor(void) {
    chat_state_t s1 = make_state(10);
    chatview_t *cv = chatview_init(&s1, "test");
    /* Cursor starts at 9 (last message) */

    /* Update with fewer messages (simulating archive) */
    chat_state_t s2 = make_state(3);
    chatview_update(cv, &s2);

    TEST_ASSERT(cv->cursor == 2,
                "cursor = %d, expected 2 (clamped to last)", cv->cursor);

    chatview_free(cv);
    TEST_PASS("chatview_update clamps cursor on count drop");
}

static void test_new_count(void) {
    chat_state_t s1 = make_state(5);
    chatview_t *cv = chatview_init(&s1, "test");

    TEST_ASSERT(chatview_new_count(cv) == 0,
                "new_count = %d, expected 0", chatview_new_count(cv));

    chat_state_t s2 = make_state(8);
    chatview_update(cv, &s2);

    TEST_ASSERT(chatview_new_count(cv) == 3,
                "new_count = %d, expected 3", chatview_new_count(cv));

    chatview_free(cv);
    TEST_PASS("chatview_new_count tracks since init");
}

static int dummy_handler(chatview_t *cv, int key, void *ud) {
    (void)cv; (void)key; (void)ud;
    return CHATVIEW_KEY_UNHANDLED;
}

static void test_set_key_handler(void) {
    chat_state_t s = make_state(3);
    chatview_t *cv = chatview_init(&s, "test");

    int userdata = 42;
    chatview_set_key_handler(cv, dummy_handler, &userdata);
    TEST_ASSERT(cv->key_handler == dummy_handler,
                "key_handler not set");
    TEST_ASSERT(cv->key_handler_data == &userdata,
                "key_handler_data not set");

    chatview_free(cv);
    TEST_PASS("chatview_set_key_handler stores callback");
}

static void test_set_status(void) {
    chat_state_t s = make_state(3);
    chatview_t *cv = chatview_init(&s, "test");

    chatview_set_status(cv, "Found %d matches", 42);
    TEST_ASSERT(strcmp(cv->status, "Found 42 matches") == 0,
                "status = '%s', expected 'Found 42 matches'", cv->status);

    chatview_free(cv);
    TEST_PASS("chatview_set_status formats correctly");
}

static void test_content_rows(void) {
    chat_state_t s = make_state(3);
    chatview_t *cv = chatview_init(&s, "test");

    cv->term_rows = 24;
    TEST_ASSERT(chatview_content_rows(cv) == 21,
                "content_rows = %d, expected 21 (24 - 3)",
                chatview_content_rows(cv));

    cv->term_rows = 50;
    TEST_ASSERT(chatview_content_rows(cv) == 47,
                "content_rows = %d, expected 47",
                chatview_content_rows(cv));

    chatview_free(cv);
    TEST_PASS("chatview_content_rows calculation");
}

static void test_msg_flags_deleted(void) {
    chat_state_t s = make_state(5);
    chatview_t *cv = chatview_init(&s, "test");

    /* All flags start at 0 */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT(cv->msg_flags[i] == 0,
                    "msg_flags[%d] = %d, expected 0", i, cv->msg_flags[i]);
    }

    /* Set deleted */
    cv->msg_flags[2] |= CHATVIEW_MSG_DELETED;
    TEST_ASSERT(cv->msg_flags[2] & CHATVIEW_MSG_DELETED,
                "msg_flags[2] should have DELETED set");

    /* Clear deleted */
    cv->msg_flags[2] &= ~CHATVIEW_MSG_DELETED;
    TEST_ASSERT(!(cv->msg_flags[2] & CHATVIEW_MSG_DELETED),
                "msg_flags[2] should have DELETED cleared");

    chatview_free(cv);
    TEST_PASS("msg_flags CHATVIEW_MSG_DELETED flag");
}

static void dummy_poll(chatview_t *cv, void *ud) {
    (void)cv; (void)ud;
}

static void test_set_poll(void) {
    chat_state_t s = make_state(3);
    chatview_t *cv = chatview_init(&s, "test");

    chatview_set_poll(cv, dummy_poll, NULL);
    TEST_ASSERT(cv->poll_fn == dummy_poll, "poll_fn not set");
    TEST_ASSERT(cv->poll_data == NULL, "poll_data should be NULL");

    chatview_free(cv);
    TEST_PASS("chatview_set_poll stores callback");
}

static void test_init_zero_messages(void) {
    chat_state_t s = make_state(0);
    chatview_t *cv = chatview_init(&s, "empty");
    TEST_ASSERT(cv != NULL, "chatview_init returned NULL for 0 messages");
    TEST_ASSERT(cv->state.message_count == 0,
                "message_count = %d, expected 0", cv->state.message_count);
    TEST_ASSERT(cv->cursor == 0,
                "cursor = %d, expected 0", cv->cursor);
    /* msg_flags can be NULL for 0 messages */
    chatview_free(cv);
    TEST_PASS("chatview_init with zero messages");
}

/* --- Main --- */

int main(void) {
    printf("test_chatview_unit\n");

    /* Init colour table (required by render functions) */
    render_init();

    test_init_free();
    test_init_cursor_at_end();
    test_search_forward();
    test_search_backward();
    test_search_forward_wraps();
    test_search_sets_pattern();
    test_update_grows_flags();
    test_update_clamps_cursor();
    test_new_count();
    test_set_key_handler();
    test_set_status();
    test_content_rows();
    test_msg_flags_deleted();
    test_set_poll();
    test_init_zero_messages();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
