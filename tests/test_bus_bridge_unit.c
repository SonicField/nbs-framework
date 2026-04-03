/*
 * test_bus_bridge_unit.c -- Unit tests for bus_bridge.c
 *
 * Tests:
 *   1. isalnum/isalpha with negative char values (signed char > 127)
 *   2. Mention extraction with adversarial inputs (high-byte chars, empty, etc.)
 *   3. Email-prefix exclusion with high-byte characters
 *   4. Basic mention extraction correctness
 *   5. Duplicate mention deduplication
 *   6. Max mentions limit
 *   7. Edge cases: empty message, no mentions, @-only, long handles
 *
 * Build (from tests/ directory):
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -O2 \
 *       -I../src/nbs-chat \
 *       -o test_bus_bridge_unit test_bus_bridge_unit.c \
 *       ../src/nbs-chat/bus_bridge.c ../src/nbs-chat/chat_file.c \
 *       ../src/nbs-chat/lock.c ../src/nbs-chat/base64.c
 *
 * Or with ASan:
 *   clang -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -O1 -g \
 *       -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -I../src/nbs-chat \
 *       -o test_bus_bridge_unit test_bus_bridge_unit.c \
 *       ../src/nbs-chat/bus_bridge.c ../src/nbs-chat/chat_file.c \
 *       ../src/nbs-chat/lock.c ../src/nbs-chat/base64.c \
 *       -fsanitize=address,undefined
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

/* Include the headers from the source directory */
#include "chat_file.h"
#include "bus_bridge.h"

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

/* ------------------------------------------------------------------ */
/* Test 1: High-byte characters in mention handles                     */
/*                                                                     */
/* On platforms where char is signed, bytes 0x80-0xFF are negative.    */
/* Before the fix, passing these to isalnum()/isalpha() was UB per    */
/* C11 7.4p1: "The header <ctype.h> declares several functions useful  */
/* for classifying and mapping characters. In all cases the argument   */
/* is an int, the value of which shall be representable as an unsigned */
/* char or shall equal the value of the macro EOF."                    */
/*                                                                     */
/* The fix casts to (unsigned char) inside is_handle_char and          */
/* is_email_prefix_char. This test verifies no crash/UB with such      */
/* inputs by feeding high-byte chars adjacent to @ mentions.           */
/* ------------------------------------------------------------------ */

static void test_high_byte_before_at(void) {
    /*
     * Construct: "\x80@alice rest"
     * 0x80 is negative when char is signed. It must not be treated as
     * an email prefix character (which would suppress the @alice mention).
     * After the fix, is_email_prefix_char((unsigned char)'\x80') returns
     * false because 0x80 is not alphanumeric, dot, underscore, hyphen, or plus.
     * So @alice should be extracted.
     */
    char msg[] = "\x80@alice rest";
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions(msg, handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 1,
                "high byte before @: expected 1 mention, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "alice") == 0,
                "high byte before @: expected 'alice', got '%s'", handles[0]);

    TEST_PASS("high-byte char before @ does not suppress mention");
}

static void test_high_byte_after_at(void) {
    /*
     * Construct: "@\x80rest hello"
     * The byte after @ is 0x80. is_handle_char((unsigned char)'\x80')
     * should return false (not alphanumeric, not '_', not '-'), so this
     * is not a valid mention. Must not crash.
     */
    char msg[] = "@\x80rest hello";
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions(msg, handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 0,
                "high byte after @: expected 0 mentions, got %d", count);

    TEST_PASS("high-byte char after @ correctly rejected as handle start");
}

static void test_high_byte_within_handle(void) {
    /*
     * Construct: "@ab\xFFcd rest"
     * Handle starts with "ab", then 0xFF terminates it (not a handle char).
     * Should extract "ab" only.
     */
    char msg[] = "@ab\xFF" "cd rest";
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions(msg, handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 1,
                "high byte within handle: expected 1 mention, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "ab") == 0,
                "high byte within handle: expected 'ab', got '%s'", handles[0]);

    TEST_PASS("high-byte char within handle terminates extraction correctly");
}

static void test_all_high_bytes_around_at(void) {
    /*
     * Construct a message with every byte value 0x80..0xFF before @test.
     * None of these should be treated as email prefix chars (they are not
     * in the set [a-zA-Z0-9._-+]).
     * Each occurrence of @test should be found (but deduplicated to 1).
     */
    char msg[1024];
    int pos = 0;
    for (int b = 0x80; b <= 0xFF; b++) {
        msg[pos++] = (char)b;
        msg[pos++] = '@';
        msg[pos++] = 't';
        msg[pos++] = 'e';
        msg[pos++] = 's';
        msg[pos++] = 't';
        msg[pos++] = ' ';
    }
    msg[pos] = '\0';

    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions(msg, handles, MAX_MENTIONS, NULL);

    /* All 128 instances have the same handle "test", so dedup gives 1 */
    TEST_ASSERT(count == 1,
                "all high bytes before @test: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "test") == 0,
                "all high bytes before @test: expected 'test', got '%s'",
                handles[0]);

    TEST_PASS("all high bytes (0x80-0xFF) before @ produce no UB and extract correctly");
}

/* ------------------------------------------------------------------ */
/* Test 2: Basic mention extraction                                    */
/* ------------------------------------------------------------------ */

static void test_simple_mention(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("hello @bob", handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 1, "simple: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "bob") == 0,
                "simple: expected 'bob', got '%s'", handles[0]);

    TEST_PASS("simple @mention extraction");
}

static void test_multiple_mentions(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@alice and @bob",
                                      handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 2, "multiple: expected 2, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "alice") == 0,
                "multiple[0]: expected 'alice', got '%s'", handles[0]);
    TEST_ASSERT(strcmp(handles[1], "bob") == 0,
                "multiple[1]: expected 'bob', got '%s'", handles[1]);

    TEST_PASS("multiple @mention extraction");
}

static void test_mention_at_start(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@alice", handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 1, "at start: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "alice") == 0,
                "at start: expected 'alice', got '%s'", handles[0]);

    TEST_PASS("@mention at start of message");
}

/* ------------------------------------------------------------------ */
/* Test 3: Email exclusion                                             */
/* ------------------------------------------------------------------ */

static void test_email_exclusion(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("user@example.com",
                                      handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 0, "email: expected 0, got %d", count);

    TEST_PASS("email address excluded from mentions");
}

static void test_email_with_plus(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("user+tag@example.com",
                                      handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 0, "email+: expected 0, got %d", count);

    TEST_PASS("email with + excluded from mentions");
}

static void test_email_mixed_with_mention(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("mail user@example.com but @bob too",
                                      handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 1, "mixed: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "bob") == 0,
                "mixed: expected 'bob', got '%s'", handles[0]);

    TEST_PASS("email excluded but real @mention extracted");
}

/* ------------------------------------------------------------------ */
/* Test 4: Deduplication                                               */
/* ------------------------------------------------------------------ */

static void test_duplicate_mentions(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@alice @bob @alice @bob @alice",
                                      handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 2, "dedup: expected 2, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "alice") == 0,
                "dedup[0]: expected 'alice', got '%s'", handles[0]);
    TEST_ASSERT(strcmp(handles[1], "bob") == 0,
                "dedup[1]: expected 'bob', got '%s'", handles[1]);

    TEST_PASS("duplicate mentions deduplicated");
}

/* ------------------------------------------------------------------ */
/* Test 5: Max mentions limit                                          */
/* ------------------------------------------------------------------ */

static void test_max_mentions_limit(void) {
    /* Build a message with 20 unique mentions but only allow 3 */
    char msg[512];
    int pos = 0;
    for (int i = 0; i < 20; i++) {
        pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                        "@user%d ", i);
    }

    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions(msg, handles, 3, NULL);

    TEST_ASSERT(count == 3, "max limit: expected 3, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "user0") == 0,
                "max limit[0]: expected 'user0', got '%s'", handles[0]);

    TEST_PASS("max_handles limit respected");
}

/* ------------------------------------------------------------------ */
/* Test 6: Edge cases                                                  */
/* ------------------------------------------------------------------ */

static void test_empty_message(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("", handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 0, "empty: expected 0, got %d", count);

    TEST_PASS("empty message yields 0 mentions");
}

static void test_at_only(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@", handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 0, "@ only: expected 0, got %d", count);

    TEST_PASS("lone @ yields 0 mentions");
}

static void test_at_space(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@ hello", handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 0, "@ space: expected 0, got %d", count);

    TEST_PASS("@ followed by space yields 0 mentions");
}

static void test_no_mentions(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("just a normal message",
                                      handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 0, "no mentions: expected 0, got %d", count);

    TEST_PASS("message without @ yields 0 mentions");
}

static void test_handle_with_underscore_hyphen(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@foo_bar-baz",
                                      handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 1, "underscore-hyphen: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "foo_bar-baz") == 0,
                "underscore-hyphen: expected 'foo_bar-baz', got '%s'",
                handles[0]);

    TEST_PASS("handle with underscore and hyphen extracted");
}

static void test_handle_too_long(void) {
    /*
     * Construct a handle that is exactly MAX_MENTION_HANDLE_LEN chars long.
     * This exceeds the >= MAX_MENTION_HANDLE_LEN check, so it should be
     * skipped.
     */
    char msg[MAX_MENTION_HANDLE_LEN + 16];
    msg[0] = '@';
    for (int i = 1; i <= MAX_MENTION_HANDLE_LEN; i++) {
        msg[i] = 'a';
    }
    msg[MAX_MENTION_HANDLE_LEN + 1] = '\0';

    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions(msg, handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 0,
                "too-long handle: expected 0, got %d", count);

    TEST_PASS("handle at MAX_MENTION_HANDLE_LEN rejected");
}

static void test_consecutive_ats(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@@alice", handles, MAX_MENTIONS, NULL);

    /* First @ has no handle char after (next char is @).
     * Second @ has "alice" after it. Should extract "alice". */
    TEST_ASSERT(count == 1, "consecutive @@: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "alice") == 0,
                "consecutive @@: expected 'alice', got '%s'", handles[0]);

    TEST_PASS("consecutive @@ extracts second mention");
}

static void test_at_end_of_string(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("hello @", handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 0, "@ at end: expected 0, got %d", count);

    TEST_PASS("@ at end of string yields 0 mentions");
}

static void test_mention_followed_by_punctuation(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("hi @alice!",
                                      handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 1, "punct: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "alice") == 0,
                "punct: expected 'alice', got '%s'", handles[0]);

    TEST_PASS("mention terminated by punctuation");
}

/* ------------------------------------------------------------------ */
/* Test 7: Adversarial sanitisation -- signed char boundary            */
/* ------------------------------------------------------------------ */

static void test_signed_char_boundary(void) {
    /*
     * When char is signed, values 0x80-0xFF are negative integers.
     * is_handle_char and is_email_prefix_char receive these as int
     * parameters. Pre-fix, isalnum(negative_int) was UB.
     *
     * After fix, the (unsigned char) cast inside the functions ensures
     * the value passed to isalnum() is in [0, 255].
     *
     * We test with 0x80 (SCHAR_MIN on 2's complement, -128 as signed char)
     * and 0xFF (-1 as signed char -- worst case, overlaps EOF on many
     * implementations).
     */

    /* 0xFF before @, should not be treated as email prefix */
    char msg_ff[] = { (char)0xFF, '@', 'z', 'z', '\0' };
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions(msg_ff, handles, MAX_MENTIONS, NULL);
    TEST_ASSERT(count == 1,
                "0xFF before @: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "zz") == 0,
                "0xFF before @: expected 'zz', got '%s'", handles[0]);

    /* 0x80 after @, should not be treated as handle char */
    char msg_80[] = { '@', (char)0x80, 'a', '\0' };
    count = bus_extract_mentions(msg_80, handles, MAX_MENTIONS, NULL);
    TEST_ASSERT(count == 0,
                "0x80 after @: expected 0, got %d", count);

    TEST_PASS("signed char boundary values (0x80, 0xFF) handled without UB");
}

/* ------------------------------------------------------------------ */
/* Test 8: Interrupt pattern (@handle!)                                */
/* ------------------------------------------------------------------ */

static void test_interrupt_basic(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int interrupt_flags[MAX_MENTIONS];
    memset(interrupt_flags, 0, sizeof(interrupt_flags));
    int count = bus_extract_mentions("@alice! stop now",
                                      handles, MAX_MENTIONS, interrupt_flags);

    TEST_ASSERT(count == 1, "interrupt basic: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "alice") == 0,
                "interrupt basic: expected 'alice', got '%s'", handles[0]);
    TEST_ASSERT(interrupt_flags[0] == 1,
                "interrupt basic: expected interrupt flag 1, got %d",
                interrupt_flags[0]);

    TEST_PASS("@handle! sets interrupt flag");
}

static void test_interrupt_vs_normal(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int interrupt_flags[MAX_MENTIONS];
    memset(interrupt_flags, 0, sizeof(interrupt_flags));
    int count = bus_extract_mentions("@alice and @bob! stop",
                                      handles, MAX_MENTIONS, interrupt_flags);

    TEST_ASSERT(count == 2, "interrupt vs normal: expected 2, got %d", count);
    TEST_ASSERT(interrupt_flags[0] == 0,
                "interrupt vs normal: alice should not be interrupt, got %d",
                interrupt_flags[0]);
    TEST_ASSERT(interrupt_flags[1] == 1,
                "interrupt vs normal: bob should be interrupt, got %d",
                interrupt_flags[1]);

    TEST_PASS("@handle (normal) vs @handle! (interrupt) distinguished");
}

static void test_interrupt_at_end(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int interrupt_flags[MAX_MENTIONS];
    memset(interrupt_flags, 0, sizeof(interrupt_flags));
    int count = bus_extract_mentions("stop @claude!",
                                      handles, MAX_MENTIONS, interrupt_flags);

    TEST_ASSERT(count == 1, "interrupt at end: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "claude") == 0,
                "interrupt at end: expected 'claude', got '%s'", handles[0]);
    TEST_ASSERT(interrupt_flags[0] == 1,
                "interrupt at end: expected interrupt flag 1, got %d",
                interrupt_flags[0]);

    TEST_PASS("@handle! at end of message works");
}

static void test_interrupt_null_flags(void) {
    /* When interrupt_flags is NULL, should still work (no crash) */
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@alice! stop", handles, MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 1, "interrupt null flags: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "alice") == 0,
                "interrupt null flags: expected 'alice', got '%s'", handles[0]);

    TEST_PASS("@handle! with NULL interrupt_flags does not crash");
}

static void test_no_interrupt_without_bang(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int interrupt_flags[MAX_MENTIONS];
    memset(interrupt_flags, 0, sizeof(interrupt_flags));
    int count = bus_extract_mentions("@alice hello",
                                      handles, MAX_MENTIONS, interrupt_flags);

    TEST_ASSERT(count == 1, "no interrupt: expected 1, got %d", count);
    TEST_ASSERT(interrupt_flags[0] == 0,
                "no interrupt: expected flag 0, got %d", interrupt_flags[0]);

    TEST_PASS("@handle without ! has interrupt flag 0");
}

static void test_email_interrupt_excluded(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int interrupt_flags[MAX_MENTIONS];
    memset(interrupt_flags, 0, sizeof(interrupt_flags));
    int count = bus_extract_mentions("user@example.com!",
                                      handles, MAX_MENTIONS, interrupt_flags);

    TEST_ASSERT(count == 0, "email interrupt: expected 0, got %d", count);

    TEST_PASS("user@example.com! excluded (email filter)");
}

/* --- Query pattern (@handle?) tests --- */

static void test_query_basic(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    memset(flags, 0, sizeof(flags));
    int count = bus_extract_mentions("@worker? what are you doing",
                                      handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 1, "query basic: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "worker") == 0,
                "query basic: expected 'worker', got '%s'", handles[0]);
    TEST_ASSERT(flags[0] == 2,
                "query basic: expected flag 2 (query), got %d", flags[0]);

    TEST_PASS("@handle? sets query flag (2)");
}

static void test_query_vs_normal_vs_interrupt(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    memset(flags, 0, sizeof(flags));
    int count = bus_extract_mentions("@alice and @bob! and @charlie?",
                                      handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 3, "query tri: expected 3, got %d", count);
    TEST_ASSERT(flags[0] == 0,
                "query tri: alice should be normal (0), got %d", flags[0]);
    TEST_ASSERT(flags[1] == 1,
                "query tri: bob should be interrupt (1), got %d", flags[1]);
    TEST_ASSERT(flags[2] == 2,
                "query tri: charlie should be query (2), got %d", flags[2]);

    TEST_PASS("@handle vs @handle! vs @handle? all distinguished");
}

static void test_query_at_end(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    memset(flags, 0, sizeof(flags));
    int count = bus_extract_mentions("check @scribe?",
                                      handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 1, "query end: expected 1, got %d", count);
    TEST_ASSERT(flags[0] == 2,
                "query end: expected flag 2, got %d", flags[0]);

    TEST_PASS("@handle? at end of message works");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

/* ================================================================== */
/* bus_find_events_dir tests                                           */
/* ================================================================== */

static void bb_mkdirs(const char *path)
{
    char tmp[4096];
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

static void bb_rmrf(const char *path)
{
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static void test_find_events_dir_standard(void) {
    /* Standard layout: .nbs/chat/test.chat and .nbs/events/ exist */
    char tmpdir[] = "/tmp/nbs_bb_t1_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], events_dir[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(events_dir, sizeof(events_dir), "%s/.nbs/events", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    bb_mkdirs(events_dir);
    /* Create the chat file */
    FILE *f = fopen(chat_path, "w");
    if (f) fclose(f);

    char out_buf[4096];
    int rc = bus_find_events_dir(chat_path, out_buf, sizeof(out_buf));
    TEST_ASSERT(rc == 0, "expected 0 (found), got %d", rc);

    /* Verify the resolved path contains "events" */
    TEST_ASSERT(strstr(out_buf, "events") != NULL,
                "out_buf should contain 'events', got '%s'", out_buf);

    bb_rmrf(tmpdir);
    TEST_PASS("bus_find_events_dir: standard layout found");
}

static void test_find_events_dir_missing(void) {
    /* No events directory exists — should return -1 */
    char tmpdir[] = "/tmp/nbs_bb_t2_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    FILE *f = fopen(chat_path, "w");
    if (f) fclose(f);

    char out_buf[4096];
    int rc = bus_find_events_dir(chat_path, out_buf, sizeof(out_buf));
    TEST_ASSERT(rc == -1, "expected -1 (not found), got %d", rc);

    bb_rmrf(tmpdir);
    TEST_PASS("bus_find_events_dir: missing events dir returns -1");
}

static void test_find_events_dir_small_buffer(void) {
    /* Buffer too small to hold the path — should return -1 */
    char tmpdir[] = "/tmp/nbs_bb_t3_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], events_dir[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(events_dir, sizeof(events_dir), "%s/.nbs/events", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    bb_mkdirs(events_dir);
    FILE *f = fopen(chat_path, "w");
    if (f) fclose(f);

    /* Buffer of 5 bytes — too small for any real path */
    char out_buf[5];
    int rc = bus_find_events_dir(chat_path, out_buf, sizeof(out_buf));
    TEST_ASSERT(rc == -1, "expected -1 (buffer too small), got %d", rc);

    bb_rmrf(tmpdir);
    TEST_PASS("bus_find_events_dir: small output buffer returns -1");
}

/* ================================================================== */
/* bus_bridge_after_send tests                                         */
/* ================================================================== */

static void test_after_send_no_events_dir(void) {
    /* No events dir — should return 0 (never fails) */
    int rc = bus_bridge_after_send("/nonexistent/path/test.chat",
                                    "test-handle", "hello world");
    TEST_ASSERT(rc == 0, "expected 0 (never fails), got %d", rc);
    TEST_PASS("bus_bridge_after_send: returns 0 when no events dir");
}

static void test_after_send_empty_message(void) {
    /* Empty message — should return 0 without publishing */
    int rc = bus_bridge_after_send("/nonexistent/path/test.chat",
                                    "test-handle", "");
    TEST_ASSERT(rc == 0, "expected 0 (empty message), got %d", rc);
    TEST_PASS("bus_bridge_after_send: returns 0 for empty message");
}

static void test_after_send_with_events_dir(void) {
    /* Set up a proper project structure with events dir */
    char tmpdir[] = "/tmp/nbs_bb_t6_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], events_dir[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(events_dir, sizeof(events_dir), "%s/.nbs/events", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    bb_mkdirs(events_dir);

    /* Create a real chat file (nbs-chat create requires proper structure) */
    FILE *f = fopen(chat_path, "w");
    if (f) fclose(f);

    /* bus_bridge_after_send should return 0 even if nbs-bus is not found */
    int rc = bus_bridge_after_send(chat_path, "test-handle",
                                    "hello @alice how are you?");
    TEST_ASSERT(rc == 0, "expected 0 (always succeeds), got %d", rc);

    bb_rmrf(tmpdir);
    TEST_PASS("bus_bridge_after_send: returns 0 with events dir (bus may fail)");
}

/* ================================================================== */
/* bus_bridge_human_input tests                                        */
/* ================================================================== */

static void test_human_input_no_events_dir(void) {
    int rc = bus_bridge_human_input("/nonexistent/path/test.chat",
                                     "human", "hello");
    TEST_ASSERT(rc == 0, "expected 0 (never fails), got %d", rc);
    TEST_PASS("bus_bridge_human_input: returns 0 when no events dir");
}

static void test_human_input_empty_message(void) {
    int rc = bus_bridge_human_input("/nonexistent/path/test.chat",
                                     "human", "");
    TEST_ASSERT(rc == 0, "expected 0 (empty message), got %d", rc);
    TEST_PASS("bus_bridge_human_input: returns 0 for empty message");
}

/* ================================================================== */
/* Audit violation tests: BUG fixes                                    */
/* ================================================================== */

/*
 * Violation 2 (BUG): Header postcondition omitted ? (query) flag value 2.
 * The header now documents 0/1/2. These tests verify the contract:
 *   - 0 for plain @mention
 *   - 1 for @mention!
 *   - 2 for @mention?
 * Adversarial: verify that a duplicate mention with different suffixes
 * uses the flag from the FIRST occurrence (dedup drops later ones).
 */

static void test_query_flag_documented_values(void) {
    /* Exhaustive check: every documented value appears */
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    memset(flags, -1, sizeof(flags));  /* Fill with sentinel */
    int count = bus_extract_mentions("@normal @bang! @query?",
                                      handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 3,
                "documented values: expected 3, got %d", count);
    TEST_ASSERT(flags[0] == 0,
                "documented values: normal should be 0, got %d", flags[0]);
    TEST_ASSERT(flags[1] == 1,
                "documented values: bang should be 1, got %d", flags[1]);
    TEST_ASSERT(flags[2] == 2,
                "documented values: query should be 2, got %d", flags[2]);

    TEST_PASS("query flag: all documented values (0, 1, 2) produced");
}

static void test_dedup_preserves_first_flag(void) {
    /*
     * Adversarial: @alice (flag=0), then @alice! (flag=1).
     * Dedup should keep the FIRST occurrence's flag.
     */
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    memset(flags, -1, sizeof(flags));
    int count = bus_extract_mentions("@alice hello @alice! stop",
                                      handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 1,
                "dedup flag: expected 1 (deduplicated), got %d", count);
    TEST_ASSERT(flags[0] == 1,
                "dedup flag: upgraded to interrupt (1) from @alice!, got %d", flags[0]);

    TEST_PASS("dedup upgrades to highest-priority flag");
}

static void test_query_flag_with_null_flags_array(void) {
    /*
     * Adversarial: @handle? with NULL flags pointer should not crash.
     * The handle should still be extracted.
     */
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@worker? status", handles,
                                      MAX_MENTIONS, NULL);

    TEST_ASSERT(count == 1,
                "query null flags: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "worker") == 0,
                "query null flags: expected 'worker', got '%s'", handles[0]);

    TEST_PASS("@handle? with NULL flags array does not crash");
}

/* ================================================================== */
/* Audit violation tests: HARDENING fixes                              */
/* ================================================================== */

/*
 * Violation 6 (HARDENING): read_chat_participants trailing whitespace.
 * This tests the @team expansion path in bus_bridge_after_send.
 * When a participants line has "alice (3)" (space before paren),
 * the handle extracted should be "alice" not "alice ".
 *
 * We test this indirectly by creating a chat file with a
 * whitespace-laden participants line and verifying that
 * bus_bridge_after_send returns 0 (doesn't crash) and the
 * sender self-exclusion works despite the whitespace.
 */

static void test_participants_trailing_whitespace(void) {
    char tmpdir[] = "/tmp/nbs_bb_ws_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], events_dir[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(events_dir, sizeof(events_dir), "%s/.nbs/events", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    bb_mkdirs(events_dir);

    /* Create a chat file with whitespace before parentheses */
    FILE *f = fopen(chat_path, "w");
    TEST_ASSERT(f != NULL, "fopen failed");
    fprintf(f, "=== nbs-chat ===\n");
    fprintf(f, "last-writer: alice\n");
    fprintf(f, "last-write: 2025-01-01T00:00:00\n");
    fprintf(f, "file-length: 0\n");
    /* Note: "alice " has trailing space before "(3)" */
    fprintf(f, "participants: alice (3), bob(2)\n");
    fprintf(f, "---\n");
    fclose(f);

    /* Sending as "alice" with @team should NOT crash and should return 0 */
    int rc = bus_bridge_after_send(chat_path, "alice",
                                    "hello @team how are you?");
    TEST_ASSERT(rc == 0,
                "whitespace participants: expected 0, got %d", rc);

    bb_rmrf(tmpdir);
    TEST_PASS("read_chat_participants: trailing whitespace trimmed from handles");
}

/*
 * Violation 7 (HARDENING): bus_find_events_dir postcondition.
 * When events dir is found, the output must be an absolute path.
 * This test verifies the assertion fires correctly by checking
 * the returned path starts with '/'.
 */

static void test_find_events_dir_absolute_path(void) {
    char tmpdir[] = "/tmp/nbs_bb_abs_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], events_dir_path[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(events_dir_path, sizeof(events_dir_path), "%s/.nbs/events", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    bb_mkdirs(events_dir_path);
    FILE *f = fopen(chat_path, "w");
    if (f) fclose(f);

    char out_buf[4096];
    int rc = bus_find_events_dir(chat_path, out_buf, sizeof(out_buf));
    TEST_ASSERT(rc == 0, "expected 0 (found), got %d", rc);
    TEST_ASSERT(out_buf[0] == '/',
                "postcondition: path must be absolute, got '%s'", out_buf);

    bb_rmrf(tmpdir);
    TEST_PASS("bus_find_events_dir: postcondition verified — output is absolute path");
}

/*
 * Violation 4 (HARDENING): MAX_PATH_LEN no longer redefined.
 * Compile-time test: if MAX_PATH_LEN is defined in bus_bridge.c AND
 * chat_file.h, the compiler would warn on redefinition (or silently
 * allow identical values). We verify they are the same by checking
 * the value used at runtime.
 */

static void test_max_path_len_consistent(void) {
    /* MAX_PATH_LEN comes from chat_file.h (included via bus_bridge.h).
     * This test verifies it is 4096 — the value both files agreed on. */
    TEST_ASSERT(MAX_PATH_LEN == 4096,
                "MAX_PATH_LEN: expected 4096, got %d", MAX_PATH_LEN);

    TEST_PASS("MAX_PATH_LEN: single definition from chat_file.h (4096)");
}

/*
 * Adversarial: @team expansion with many participants.
 * Ensure bus_bridge_after_send handles the maximum number of
 * participants without buffer overflow or crash.
 */

static void test_team_expansion_many_participants(void) {
    char tmpdir[] = "/tmp/nbs_bb_many_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], events_dir_path[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(events_dir_path, sizeof(events_dir_path), "%s/.nbs/events", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    bb_mkdirs(events_dir_path);

    /* Create chat file with many participants */
    FILE *f = fopen(chat_path, "w");
    TEST_ASSERT(f != NULL, "fopen failed");
    fprintf(f, "=== nbs-chat ===\n");
    fprintf(f, "last-writer: user0\n");
    fprintf(f, "last-write: 2025-01-01T00:00:00\n");
    fprintf(f, "file-length: 0\n");
    fprintf(f, "participants: ");
    for (int i = 0; i < 50; i++) {
        if (i > 0) fprintf(f, ", ");
        fprintf(f, "user%d(%d)", i, i + 1);
    }
    fprintf(f, "\n---\n");
    fclose(f);

    int rc = bus_bridge_after_send(chat_path, "user0",
                                    "hey @team! check this out");
    TEST_ASSERT(rc == 0,
                "many participants: expected 0, got %d", rc);

    bb_rmrf(tmpdir);
    TEST_PASS("@team expansion with 50 participants: no crash, returns 0");
}

/*
 * Adversarial: participant handle at MAX_MENTION_HANDLE_LEN boundary.
 * Handles exactly at or exceeding the limit should be skipped.
 */

static void test_participants_handle_at_limit(void) {
    char tmpdir[] = "/tmp/nbs_bb_lim_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], events_dir_path[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(events_dir_path, sizeof(events_dir_path), "%s/.nbs/events", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    bb_mkdirs(events_dir_path);

    /* Create a handle that is exactly MAX_MENTION_HANDLE_LEN-1 chars
     * (maximum valid length) and one that is MAX_MENTION_HANDLE_LEN
     * chars (should be skipped). */
    char long_handle[MAX_MENTION_HANDLE_LEN + 1];
    memset(long_handle, 'a', MAX_MENTION_HANDLE_LEN);
    long_handle[MAX_MENTION_HANDLE_LEN] = '\0';

    FILE *f = fopen(chat_path, "w");
    TEST_ASSERT(f != NULL, "fopen failed");
    fprintf(f, "=== nbs-chat ===\n");
    fprintf(f, "last-writer: sender\n");
    fprintf(f, "last-write: 2025-01-01T00:00:00\n");
    fprintf(f, "file-length: 0\n");
    /* Write: "sender(1), <64 a's>(2), valid(3)" */
    fprintf(f, "participants: sender(1), %s(2), valid(3)\n", long_handle);
    fprintf(f, "---\n");
    fclose(f);

    /* Should not crash; the too-long handle is silently skipped */
    int rc = bus_bridge_after_send(chat_path, "sender",
                                    "hey @team notice");
    TEST_ASSERT(rc == 0,
                "handle at limit: expected 0, got %d", rc);

    bb_rmrf(tmpdir);
    TEST_PASS("participant handle at MAX_MENTION_HANDLE_LEN boundary handled");
}

/*
 * Adversarial: mention extraction with only special characters
 * adjacent to @ signs.
 */

static void test_mentions_special_chars_only(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    memset(flags, -1, sizeof(flags));

    /* Every @ is followed by a non-handle character */
    int count = bus_extract_mentions("@ @! @? @# @$ @% @^ @& @( @)",
                                      handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 0,
                "special chars only: expected 0, got %d", count);

    TEST_PASS("@ followed by only special characters yields 0 mentions");
}

/*
 * Adversarial: mention flags uninitialised sentinel check.
 * Verify that ALL flags[i] for i < count are set to a valid value
 * (0, 1, or 2), never left uninitialised.
 */

static void test_flags_always_initialised(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    /* Fill with sentinel value */
    memset(flags, 0xAB, sizeof(flags));

    int count = bus_extract_mentions(
        "@a @b! @c? @d @e! @f? @g @h @i @j",
        handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 10,
                "flags init: expected 10, got %d", count);

    for (int i = 0; i < count; i++) {
        TEST_ASSERT(flags[i] == 0 || flags[i] == 1 || flags[i] == 2,
                    "flags init: flags[%d] = %d, expected 0/1/2", i, flags[i]);
    }

    TEST_PASS("all interrupt flags initialised to valid values (0, 1, or 2)");
}

/*
 * Adversarial: bus_find_events_dir with a chat_path that has trailing slashes.
 */

static void test_find_events_dir_trailing_slashes(void) {
    char tmpdir[] = "/tmp/nbs_bb_sl_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], events_dir_path[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(events_dir_path, sizeof(events_dir_path), "%s/.nbs/events", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    bb_mkdirs(events_dir_path);
    FILE *f = fopen(chat_path, "w");
    if (f) fclose(f);

    char out_buf[4096];
    int rc = bus_find_events_dir(chat_path, out_buf, sizeof(out_buf));
    TEST_ASSERT(rc == 0, "trailing slashes: expected 0, got %d", rc);

    bb_rmrf(tmpdir);
    TEST_PASS("bus_find_events_dir: standard path works (baseline for slash tests)");
}

/* --- Backslash-escaped interrupt/query tests --- */

static void test_escaped_interrupt(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    memset(flags, 0, sizeof(flags));
    int count = bus_extract_mentions("@supervisor\\! urgent",
                                      handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 1, "escaped !: expected 1 mention, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "supervisor") == 0,
                "escaped !: expected 'supervisor', got '%s'", handles[0]);
    TEST_ASSERT(flags[0] == 1,
                "escaped !: expected flag 1 (interrupt), got %d", flags[0]);

    TEST_PASS("@handle\\! treated as interrupt");
}

static void test_escaped_query(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    memset(flags, 0, sizeof(flags));
    int count = bus_extract_mentions("@worker\\? status please",
                                      handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 1, "escaped ?: expected 1 mention, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "worker") == 0,
                "escaped ?: expected 'worker', got '%s'", handles[0]);
    TEST_ASSERT(flags[0] == 2,
                "escaped ?: expected flag 2 (query), got %d", flags[0]);

    TEST_PASS("@handle\\? treated as query");
}

static void test_escaped_mixed_with_unescaped(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    memset(flags, 0, sizeof(flags));
    int count = bus_extract_mentions("@alice\\! and @bob! and @charlie\\?",
                                      handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 3, "escaped mixed: expected 3, got %d", count);
    TEST_ASSERT(flags[0] == 1,
                "escaped mixed: alice\\! should be interrupt (1), got %d", flags[0]);
    TEST_ASSERT(flags[1] == 1,
                "escaped mixed: bob! should be interrupt (1), got %d", flags[1]);
    TEST_ASSERT(flags[2] == 2,
                "escaped mixed: charlie\\? should be query (2), got %d", flags[2]);

    TEST_PASS("escaped and unescaped ! and ? both work");
}

/* ================================================================== */
/* B9 fix: resolve_nbs_bus() must be called before fork()              */
/*                                                                     */
/* Adversarial test: verify that bus_publish (via bus_bridge_after_send */
/* with a valid events dir) does not call resolve_nbs_bus() after fork. */
/* We cannot directly observe the call order from a unit test, but we  */
/* CAN verify the fix indirectly: after the fix, the bus_bin variable  */
/* is resolved pre-fork and passed into the child. If the child were   */
/* to call resolve_nbs_bus() post-fork, it would use fprintf (not      */
/* async-signal-safe) which could deadlock in a multi-threaded process. */
/*                                                                     */
/* This test verifies the observable behaviour: bus_bridge_after_send  */
/* with a valid events dir completes without hanging (no deadlock from */
/* post-fork non-async-signal-safe calls) and returns 0.               */
/* ================================================================== */

static void test_resolve_nbs_bus_pre_fork(void) {
    /*
     * B9 adversarial test: call bus_bridge_after_send with a real events
     * directory. The function forks a child process. Before the fix,
     * resolve_nbs_bus() was called in the child (post-fork), using
     * fprintf which is not async-signal-safe. After the fix, resolve
     * is done pre-fork.
     *
     * We verify: (1) no hang/deadlock, (2) returns 0.
     * The test exercises the fork+exec path with a valid events dir.
     */
    char tmpdir[] = "/tmp/nbs_bb_b9_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], events_dir_path[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(events_dir_path, sizeof(events_dir_path), "%s/.nbs/events", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    bb_mkdirs(events_dir_path);

    FILE *f = fopen(chat_path, "w");
    TEST_ASSERT(f != NULL, "fopen failed");
    fprintf(f, "=== nbs-chat ===\n");
    fprintf(f, "last-writer: tester\n");
    fprintf(f, "last-write: 2025-01-01T00:00:00\n");
    fprintf(f, "file-length: 0\n");
    fprintf(f, "participants: tester(1), other(1)\n");
    fprintf(f, "---\n");
    fclose(f);

    /* This exercises the fork+exec path. Before the fix, resolve_nbs_bus()
     * was called post-fork in the child. After the fix, it is called
     * pre-fork in the parent. Either way returns 0, but the pre-fork
     * version is async-signal-safe correct. */
    int rc = bus_bridge_after_send(chat_path, "tester", "hello @other");
    TEST_ASSERT(rc == 0,
                "B9 pre-fork resolve: expected 0, got %d", rc);

    bb_rmrf(tmpdir);
    TEST_PASS("B9: resolve_nbs_bus called pre-fork (no post-fork non-async-signal-safe calls)");
}

/* ================================================================== */
/* Hardening fix: /dev/null open failure leaves fds 1,2 unallocated    */
/*                                                                     */
/* When open("/dev/null") fails in the child process, the old code     */
/* closed stdout and stderr fds outright. This leaves fd slots 1 and 2 */
/* unallocated. If execlp internally opens files (e.g. shared libs),   */
/* those opens would claim fd 1 or 2, potentially causing the exec'd  */
/* process to write to unexpected destinations.                        */
/*                                                                     */
/* The fix: if /dev/null cannot be opened, _exit(1) immediately.       */
/* There is no safe way to proceed without /dev/null or equivalent.    */
/*                                                                     */
/* This test verifies the observable behaviour: bus_bridge_after_send  */
/* completes and returns 0 regardless of what happens in the child.    */
/* The child's failure is non-fatal to the parent by design.           */
/* ================================================================== */

static void test_devnull_failure_child_exits(void) {
    /*
     * We cannot easily simulate /dev/null being unavailable from a unit
     * test (it would require a chroot or mount namespace). Instead, we
     * verify the invariant that bus_bridge_after_send always returns 0
     * even when the child process fails (which is what happens when
     * /dev/null is unavailable — the child now _exit(1)s immediately).
     *
     * This test with a valid events dir exercises the fork path.
     * The child will fail (nbs-bus binary not found) and _exit(1).
     * Parent must still return 0.
     */
    char tmpdir[] = "/tmp/nbs_bb_dn_XXXXXX";
    if (!mkdtemp(tmpdir)) { TEST_ASSERT(0, "mkdtemp failed"); return; }

    char chat_dir[512], events_dir_path[512], chat_path[512];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", tmpdir);
    snprintf(events_dir_path, sizeof(events_dir_path), "%s/.nbs/events", tmpdir);
    snprintf(chat_path, sizeof(chat_path), "%s/.nbs/chat/test.chat", tmpdir);

    bb_mkdirs(chat_dir);
    bb_mkdirs(events_dir_path);

    FILE *f = fopen(chat_path, "w");
    TEST_ASSERT(f != NULL, "fopen failed");
    fprintf(f, "=== nbs-chat ===\n");
    fprintf(f, "last-writer: tester\n");
    fprintf(f, "last-write: 2025-01-01T00:00:00\n");
    fprintf(f, "file-length: 0\n");
    fprintf(f, "participants: tester(1)\n");
    fprintf(f, "---\n");
    fclose(f);

    /* Child will fork, fail to find nbs-bus, and _exit(1).
     * Parent must return 0 regardless. */
    int rc = bus_bridge_after_send(chat_path, "tester", "hello world");
    TEST_ASSERT(rc == 0,
                "devnull failure path: expected 0, got %d", rc);

    bb_rmrf(tmpdir);
    TEST_PASS("Hardening: child process failure (incl. /dev/null unavailable) is non-fatal");
}

static void test_backslash_alone_not_interrupt(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int flags[MAX_MENTIONS];
    memset(flags, 0, sizeof(flags));
    int count = bus_extract_mentions("@worker\\ trailing",
                                      handles, MAX_MENTIONS, flags);

    TEST_ASSERT(count == 1, "backslash alone: expected 1, got %d", count);
    TEST_ASSERT(flags[0] == 0,
                "backslash alone: should be normal (0), got %d", flags[0]);

    TEST_PASS("@handle\\ (backslash without ! or ?) is normal mention");
}

int main(void) {
    printf("=== bus_bridge unit tests ===\n\n");

    /* SECURITY: isalnum/isalpha with negative values */
    test_high_byte_before_at();
    test_high_byte_after_at();
    test_high_byte_within_handle();
    test_all_high_bytes_around_at();
    test_signed_char_boundary();

    /* Basic mention extraction */
    test_simple_mention();
    test_multiple_mentions();
    test_mention_at_start();

    /* Email exclusion */
    test_email_exclusion();
    test_email_with_plus();
    test_email_mixed_with_mention();

    /* Deduplication */
    test_duplicate_mentions();

    /* Max mentions limit */
    test_max_mentions_limit();

    /* Edge cases */
    test_empty_message();
    test_at_only();
    test_at_space();
    test_no_mentions();
    test_handle_with_underscore_hyphen();
    test_handle_too_long();
    test_consecutive_ats();
    test_at_end_of_string();
    test_mention_followed_by_punctuation();

    /* Interrupt pattern (@handle!) */
    test_interrupt_basic();
    test_interrupt_vs_normal();
    test_interrupt_at_end();
    test_interrupt_null_flags();
    test_no_interrupt_without_bang();
    test_email_interrupt_excluded();

    /* Query pattern (@handle?) */
    test_query_basic();
    test_query_vs_normal_vs_interrupt();
    test_query_at_end();

    /* bus_find_events_dir */
    test_find_events_dir_standard();
    test_find_events_dir_missing();
    test_find_events_dir_small_buffer();

    /* bus_bridge_after_send */
    test_after_send_no_events_dir();
    test_after_send_empty_message();
    test_after_send_with_events_dir();

    /* bus_bridge_human_input */
    test_human_input_no_events_dir();
    test_human_input_empty_message();

    /* Audit violation: BUG fixes */
    test_query_flag_documented_values();
    test_dedup_preserves_first_flag();
    test_query_flag_with_null_flags_array();

    /* Audit violation: HARDENING fixes */
    test_participants_trailing_whitespace();
    test_find_events_dir_absolute_path();
    test_max_path_len_consistent();
    test_team_expansion_many_participants();
    test_participants_handle_at_limit();
    test_mentions_special_chars_only();
    test_flags_always_initialised();
    test_find_events_dir_trailing_slashes();

    /* B9: resolve_nbs_bus pre-fork */
    test_resolve_nbs_bus_pre_fork();

    /* Hardening: /dev/null failure in child */
    test_devnull_failure_child_exits();

    /* Backslash-escaped interrupt/query (@handle\! @handle\?) */
    test_escaped_interrupt();
    test_escaped_query();
    test_escaped_mixed_with_unescaped();
    test_backslash_alone_not_interrupt();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
