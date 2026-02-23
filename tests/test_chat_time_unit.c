/*
 * test_chat_time_unit.c — Unit tests for time specification parsing.
 *
 * Falsifiable claims tested:
 *   1-4: Relative timespecs (s/m/h/d) resolve to now - offset
 *   5: Epoch (≥10 digits) passes through as-is
 *   6: ISO 8601 parses to correct epoch
 *   7-9: Invalid inputs return -1
 *   10: Zero relative ("0s") resolves to approximately now
 *   11: Large epoch value parses correctly
 */

#include "../src/nbs-chat/time_parse.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
    printf("test_chat_time_unit\n");

    time_t now = time(NULL);
    time_t result;

    /* 1. Relative seconds: "30s" → now - 30 (±2s tolerance) */
    {
        int rc = parse_timespec("30s", &result);
        CHECK("relative 30s parses", rc == 0);
        CHECK("relative 30s value",
              result >= now - 32 && result <= now - 28);
    }

    /* 2. Relative minutes: "5m" → now - 300 (±2s tolerance) */
    {
        int rc = parse_timespec("5m", &result);
        CHECK("relative 5m parses", rc == 0);
        CHECK("relative 5m value",
              result >= now - 302 && result <= now - 298);
    }

    /* 3. Relative hours: "2h" → now - 7200 (±2s tolerance) */
    {
        int rc = parse_timespec("2h", &result);
        CHECK("relative 2h parses", rc == 0);
        CHECK("relative 2h value",
              result >= now - 7202 && result <= now - 7198);
    }

    /* 4. Relative days: "1d" → now - 86400 (±2s tolerance) */
    {
        int rc = parse_timespec("1d", &result);
        CHECK("relative 1d parses", rc == 0);
        CHECK("relative 1d value",
              result >= now - 86402 && result <= now - 86398);
    }

    /* 5. Epoch: "1771834287" → exact value */
    {
        int rc = parse_timespec("1771834287", &result);
        CHECK("epoch parses", rc == 0);
        CHECK("epoch value exact", result == 1771834287);
    }

    /* 6. ISO 8601: known timestamp → correct epoch
     * We construct a known time and verify round-trip. */
    {
        /* Use localtime to build a string we know will parse correctly */
        time_t test_time = now - 3600;
        struct tm tm_buf;
        struct tm *tm = localtime_r(&test_time, &tm_buf);
        char iso[32];
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", tm);
        int rc = parse_timespec(iso, &result);
        CHECK("ISO 8601 parses", rc == 0);
        /* mktime + localtime round-trip should be exact */
        CHECK("ISO 8601 value correct", result == test_time);
    }

    /* 7. Invalid: empty string → -1 */
    {
        int rc = parse_timespec("", &result);
        CHECK("empty string rejected", rc == -1);
    }

    /* 8. Invalid: bare letters → -1 */
    {
        int rc = parse_timespec("abc", &result);
        CHECK("bare letters rejected", rc == -1);
    }

    /* 9. Invalid: negative → -1 */
    {
        int rc = parse_timespec("-5m", &result);
        CHECK("negative rejected", rc == -1);
    }

    /* 10. Zero relative: "0s" → approximately now */
    {
        int rc = parse_timespec("0s", &result);
        CHECK("zero relative parses", rc == 0);
        CHECK("zero relative ≈ now",
              result >= now - 2 && result <= now + 2);
    }

    /* 11. Large epoch: "9999999999" → correct value */
    {
        int rc = parse_timespec("9999999999", &result);
        CHECK("large epoch parses", rc == 0);
        CHECK("large epoch value", result == (time_t)9999999999LL);
    }

    printf("%d/%d passed\n", tests - fails, tests);
    return fails;
}
