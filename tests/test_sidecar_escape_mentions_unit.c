/*
 * test_sidecar_escape_mentions_unit.c — Unit tests for mention escaping.
 *
 * Falsifiable claims tested:
 *
 * escape_mentions:
 *   1. @handle? query pattern is escaped to @\handle?
 *   2. @handle! interrupt pattern is escaped to @\handle!
 *   3. Plain @handle is escaped to @\handle
 *   4. Text without @ is unchanged
 *   5. Empty string returns empty string
 *   6. Bare @ at end of string is unchanged
 *   7. @ followed by space is unchanged
 *   8. Multiple mentions are all escaped
 *   9. Consecutive @@ — only @-before-handle-char is escaped
 *  10. High-byte after @ is unchanged (not a handle char)
 *  11. Integration: escaped output yields 0 mentions from bus_extract_mentions
 *
 * sanitise_at_signs:
 *  12. All @ replaced with \xc0
 *  13. String with no @ is unchanged
 *  14. Empty string is unchanged
 */

#include "../src/nbs-sidecar/mention_escape.h"
#include "../src/nbs-chat/bus_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
    printf("test_sidecar_escape_mentions_unit\n");

    /* --- escape_mentions tests --- */

    /* 1. Query pattern: @worker? status → @\worker? status */
    {
        char *out = escape_mentions("@worker? status");
        CHECK("query @worker? escaped",
              strcmp(out, "@\\worker? status") == 0);
        free(out);
    }

    /* 2. Interrupt pattern: hello @alice! stop → hello @\alice! stop */
    {
        char *out = escape_mentions("hello @alice! stop");
        CHECK("interrupt @alice! escaped",
              strcmp(out, "hello @\\alice! stop") == 0);
        free(out);
    }

    /* 3. Plain mention: @bob how are you → @\bob how are you */
    {
        char *out = escape_mentions("@bob how are you");
        CHECK("plain @bob escaped",
              strcmp(out, "@\\bob how are you") == 0);
        free(out);
    }

    /* 4. No mentions: just text → just text */
    {
        char *out = escape_mentions("just text");
        CHECK("no mentions unchanged",
              strcmp(out, "just text") == 0);
        free(out);
    }

    /* 5. Empty string → empty string */
    {
        char *out = escape_mentions("");
        CHECK("empty string unchanged",
              strcmp(out, "") == 0);
        free(out);
    }

    /* 6. Bare @ at end → unchanged */
    {
        char *out = escape_mentions("hello @");
        CHECK("bare @ at end unchanged",
              strcmp(out, "hello @") == 0);
        free(out);
    }

    /* 7. @ followed by space → unchanged */
    {
        char *out = escape_mentions("@ hello");
        CHECK("@ space unchanged",
              strcmp(out, "@ hello") == 0);
        free(out);
    }

    /* 8. Multiple mentions: @a and @b? → @\a and @\b? */
    {
        char *out = escape_mentions("@a and @b?");
        CHECK("multiple mentions escaped",
              strcmp(out, "@\\a and @\\b?") == 0);
        free(out);
    }

    /* 9. Consecutive @@: @@alice → @@\alice
     * First @ is followed by @, which is not a handle char → no escape.
     * Second @ is followed by 'a', which is a handle char → escape. */
    {
        char *out = escape_mentions("@@alice");
        CHECK("consecutive @@ only escapes before handle char",
              strcmp(out, "@@\\alice") == 0);
        free(out);
    }

    /* 10. High-byte after @: @\x80rest → unchanged
     * 0x80 is not a handle character (not alnum, not _ or -). */
    {
        char *out = escape_mentions("@\x80rest");
        CHECK("high-byte after @ unchanged",
              strcmp(out, "@\x80rest") == 0);
        free(out);
    }

    /* 11. Integration: escaped output → bus_extract_mentions = 0 mentions */
    {
        char *out = escape_mentions("@worker? @tester! @scribe");
        char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
        int flags[MAX_MENTIONS];
        int count = bus_extract_mentions(out, handles, MAX_MENTIONS, flags);
        CHECK("integration: escaped yields 0 mentions", count == 0);
        free(out);
    }

    /* --- sanitise_at_signs tests --- */

    /* 12. All @ replaced with \xc0 */
    {
        char buf[] = "@alice and @bob";
        sanitise_at_signs(buf);
        CHECK("sanitise replaces all @",
              buf[0] == '\xc0' && buf[11] == '\xc0');
        /* Verify no @ remain */
        int found_at = 0;
        for (size_t i = 0; i < strlen(buf); i++) {
            if (buf[i] == '@') found_at = 1;
        }
        CHECK("sanitise leaves no @ behind", !found_at);
    }

    /* 13. String with no @ is unchanged */
    {
        char buf[] = "hello world";
        char expected[] = "hello world";
        sanitise_at_signs(buf);
        CHECK("sanitise no-@ unchanged",
              strcmp(buf, expected) == 0);
    }

    /* 14. Empty string is unchanged */
    {
        char buf[] = "";
        sanitise_at_signs(buf);
        CHECK("sanitise empty unchanged",
              strcmp(buf, "") == 0);
    }

    /* 15. Sync: escape_mentions and bus_extract_mentions agree on all 256 bytes.
     * For each byte value c, construct "@<c>rest" and check:
     *   - escape_mentions escapes (inserts \) iff
     *   - bus_extract_mentions would extract a mention starting with c.
     * This catches divergence between is_mention_handle_char and is_handle_char. */
    {
        int sync_failures = 0;
        for (int c = 0; c < 256; c++) {
            char input[8];
            input[0] = '@';
            input[1] = (char)c;
            input[2] = 'x';  /* Need ≥1 more handle char so extraction proceeds */
            input[3] = '\0';

            /* Does escape_mentions escape the @ ? */
            char *escaped = escape_mentions(input);
            int did_escape = (strlen(escaped) > strlen(input));
            free(escaped);

            /* Does bus_extract_mentions extract a mention from " @<c>x" ?
             * Prepend space so @ is preceded by whitespace (not email context). */
            char extract_input[8];
            extract_input[0] = ' ';
            extract_input[1] = '@';
            extract_input[2] = (char)c;
            extract_input[3] = 'x';
            extract_input[4] = ' ';
            extract_input[5] = '\0';

            char handles[MAX_MENTIONS][MAX_MENTION_HANDLE_LEN];
            int count = bus_extract_mentions(extract_input, handles,
                                              MAX_MENTIONS, NULL);
            int did_extract = (count > 0);

            if (did_escape != did_extract) {
                printf("      byte 0x%02x: escape=%d extract=%d\n",
                       c, did_escape, did_extract);
                sync_failures++;
            }
        }
        CHECK("sync: escape_mentions matches bus_extract_mentions for all 256 bytes",
              sync_failures == 0);
    }

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
