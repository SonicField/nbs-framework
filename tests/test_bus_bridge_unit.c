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
    int count = bus_extract_mentions(msg, handles, MAX_MENTIONS);

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
    int count = bus_extract_mentions(msg, handles, MAX_MENTIONS);

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
    int count = bus_extract_mentions(msg, handles, MAX_MENTIONS);

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
    int count = bus_extract_mentions(msg, handles, MAX_MENTIONS);

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
    int count = bus_extract_mentions("hello @bob", handles, MAX_MENTIONS);

    TEST_ASSERT(count == 1, "simple: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "bob") == 0,
                "simple: expected 'bob', got '%s'", handles[0]);

    TEST_PASS("simple @mention extraction");
}

static void test_multiple_mentions(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@alice and @bob",
                                      handles, MAX_MENTIONS);

    TEST_ASSERT(count == 2, "multiple: expected 2, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "alice") == 0,
                "multiple[0]: expected 'alice', got '%s'", handles[0]);
    TEST_ASSERT(strcmp(handles[1], "bob") == 0,
                "multiple[1]: expected 'bob', got '%s'", handles[1]);

    TEST_PASS("multiple @mention extraction");
}

static void test_mention_at_start(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@alice", handles, MAX_MENTIONS);

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
                                      handles, MAX_MENTIONS);

    TEST_ASSERT(count == 0, "email: expected 0, got %d", count);

    TEST_PASS("email address excluded from mentions");
}

static void test_email_with_plus(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("user+tag@example.com",
                                      handles, MAX_MENTIONS);

    TEST_ASSERT(count == 0, "email+: expected 0, got %d", count);

    TEST_PASS("email with + excluded from mentions");
}

static void test_email_mixed_with_mention(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("mail user@example.com but @bob too",
                                      handles, MAX_MENTIONS);

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
                                      handles, MAX_MENTIONS);

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
    int count = bus_extract_mentions(msg, handles, 3);

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
    int count = bus_extract_mentions("", handles, MAX_MENTIONS);

    TEST_ASSERT(count == 0, "empty: expected 0, got %d", count);

    TEST_PASS("empty message yields 0 mentions");
}

static void test_at_only(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@", handles, MAX_MENTIONS);

    TEST_ASSERT(count == 0, "@ only: expected 0, got %d", count);

    TEST_PASS("lone @ yields 0 mentions");
}

static void test_at_space(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@ hello", handles, MAX_MENTIONS);

    TEST_ASSERT(count == 0, "@ space: expected 0, got %d", count);

    TEST_PASS("@ followed by space yields 0 mentions");
}

static void test_no_mentions(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("just a normal message",
                                      handles, MAX_MENTIONS);

    TEST_ASSERT(count == 0, "no mentions: expected 0, got %d", count);

    TEST_PASS("message without @ yields 0 mentions");
}

static void test_handle_with_underscore_hyphen(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@foo_bar-baz",
                                      handles, MAX_MENTIONS);

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
    int count = bus_extract_mentions(msg, handles, MAX_MENTIONS);

    TEST_ASSERT(count == 0,
                "too-long handle: expected 0, got %d", count);

    TEST_PASS("handle at MAX_MENTION_HANDLE_LEN rejected");
}

static void test_consecutive_ats(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("@@alice", handles, MAX_MENTIONS);

    /* First @ has no handle char after (next char is @).
     * Second @ has "alice" after it. Should extract "alice". */
    TEST_ASSERT(count == 1, "consecutive @@: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "alice") == 0,
                "consecutive @@: expected 'alice', got '%s'", handles[0]);

    TEST_PASS("consecutive @@ extracts second mention");
}

static void test_at_end_of_string(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("hello @", handles, MAX_MENTIONS);

    TEST_ASSERT(count == 0, "@ at end: expected 0, got %d", count);

    TEST_PASS("@ at end of string yields 0 mentions");
}

static void test_mention_followed_by_punctuation(void) {
    char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
    int count = bus_extract_mentions("hi @alice!",
                                      handles, MAX_MENTIONS);

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
    int count = bus_extract_mentions(msg_ff, handles, MAX_MENTIONS);
    TEST_ASSERT(count == 1,
                "0xFF before @: expected 1, got %d", count);
    TEST_ASSERT(strcmp(handles[0], "zz") == 0,
                "0xFF before @: expected 'zz', got '%s'", handles[0]);

    /* 0x80 after @, should not be treated as handle char */
    char msg_80[] = { '@', (char)0x80, 'a', '\0' };
    count = bus_extract_mentions(msg_80, handles, MAX_MENTIONS);
    TEST_ASSERT(count == 0,
                "0x80 after @: expected 0, got %d", count);

    TEST_PASS("signed char boundary values (0x80, 0xFF) handled without UB");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

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

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
