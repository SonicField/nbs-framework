/*
 * test_chat_time_unit.c — Unit tests for time specification parsing.
 *
 * Falsifiable claims tested:
 *   1-4: Relative timespecs (s/m/h/d) resolve to now - offset
 *   5: Epoch (>=10 digits) passes through as-is
 *   6: ISO 8601 parses to correct epoch
 *   7-9: Invalid inputs return -1
 *   10: Large epoch value parses correctly
 *
 * Adversarial tests (audit violations):
 *   V1:  Postcondition *out > 0 holds on all success paths
 *   V2:  Zero relative ("0s", "0m", "0h", "0d") rejected as degenerate
 *   V3:  Epoch value exceeding time_t range rejected
 *   V4:  Relative offset exceeding time_t range rejected
 *   V5:  Pre-epoch ISO 8601 dates rejected
 *   V8:  (time(NULL) failure — cannot unit-test without mocking)
 *   V9:  Empty string triggers assertion (precondition), not silent -1
 *   V10: *out unchanged on error paths (no partial writes)
 */

#include "../src/nbs-chat/time_parse.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    /* --- Original tests (updated for V2/V9 changes) --- */

    /* 1. Relative seconds: "30s" -> now - 30 (+-2s tolerance) */
    {
        int rc = parse_timespec("30s", &result);
        CHECK("relative 30s parses", rc == 0);
        CHECK("relative 30s value",
              result >= now - 32 && result <= now - 28);
    }

    /* 2. Relative minutes: "5m" -> now - 300 (+-2s tolerance) */
    {
        int rc = parse_timespec("5m", &result);
        CHECK("relative 5m parses", rc == 0);
        CHECK("relative 5m value",
              result >= now - 302 && result <= now - 298);
    }

    /* 3. Relative hours: "2h" -> now - 7200 (+-2s tolerance) */
    {
        int rc = parse_timespec("2h", &result);
        CHECK("relative 2h parses", rc == 0);
        CHECK("relative 2h value",
              result >= now - 7202 && result <= now - 7198);
    }

    /* 4. Relative days: "1d" -> now - 86400 (+-2s tolerance) */
    {
        int rc = parse_timespec("1d", &result);
        CHECK("relative 1d parses", rc == 0);
        CHECK("relative 1d value",
              result >= now - 86402 && result <= now - 86398);
    }

    /* 5. Epoch: "1771834287" -> exact value */
    {
        int rc = parse_timespec("1771834287", &result);
        CHECK("epoch parses", rc == 0);
        CHECK("epoch value exact", result == 1771834287);
    }

    /* 6. ISO 8601: known timestamp -> correct epoch
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

    /*
     * 7. Empty string — V9 changed this from silent -1 to assertion
     *    (precondition violation). Cannot test without catching SIGABRT.
     *    Removed: the old test would now crash as intended.
     */

    /* 8. Invalid: bare letters -> -1 */
    {
        int rc = parse_timespec("abc", &result);
        CHECK("bare letters rejected", rc == -1);
    }

    /* 9. Invalid: negative -> -1 */
    {
        int rc = parse_timespec("-5m", &result);
        CHECK("negative rejected", rc == -1);
    }

    /* 10. Large epoch: "9999999999" -> correct value (on 64-bit time_t) */
    {
        int rc = parse_timespec("9999999999", &result);
        if (sizeof(time_t) >= 8) {
            CHECK("large epoch parses (64-bit time_t)", rc == 0);
            CHECK("large epoch value", result == (time_t)9999999999LL);
        } else {
            CHECK("large epoch rejected (32-bit time_t)", rc == -1);
        }
    }

    /* --- Adversarial tests for BUG violations --- */

    printf("\n   --- Adversarial: BUG violations ---\n");

    /*
     * V1: Postcondition *out > 0 on all success paths.
     * Test: verify every successful parse produces out > 0.
     * The postcondition assertion in the code itself would abort if
     * violated; these tests exercise the paths that reach the assertion.
     */
    {
        int rc = parse_timespec("1s", &result);
        CHECK("V1: relative 1s result > 0", rc == 0 && result > 0);
    }
    {
        int rc = parse_timespec("1m", &result);
        CHECK("V1: relative 1m result > 0", rc == 0 && result > 0);
    }
    {
        int rc = parse_timespec("1h", &result);
        CHECK("V1: relative 1h result > 0", rc == 0 && result > 0);
    }
    {
        int rc = parse_timespec("1d", &result);
        CHECK("V1: relative 1d result > 0", rc == 0 && result > 0);
    }
    {
        int rc = parse_timespec("1000000000", &result);
        CHECK("V1: epoch result > 0", rc == 0 && result > 0);
    }
    {
        /* ISO 8601 with a known post-epoch date */
        int rc = parse_timespec("2025-01-01T00:00:01", &result);
        CHECK("V1: ISO 8601 result > 0", rc == 0 && result > 0);
    }

    /*
     * V3: long long to time_t cast overflow (epoch path).
     * On a 64-bit system, time_t is 64 bits; LLONG_MAX-range values
     * would overflow strtoll anyway. But a 20-digit number is still
     * within long long range while being astronomically large.
     * This tests the range check on the epoch path.
     */
    {
        /* Exactly at the strtoll overflow boundary for 10-digit+ input:
         * "99999999999999999999" (20 digits) overflows long long,
         * strtoll returns LLONG_MAX and sets errno. The existing
         * val <= 0 check won't catch this, but val > TIME_T_MAX will.
         * Actually, strtoll on overflow returns LLONG_MAX which is > 0,
         * so without V3 fix it would have been cast and possibly truncated.
         */
        int rc = parse_timespec("99999999999999999999", &result);
        CHECK("V3: 20-digit overflow epoch rejected", rc == -1);
    }
    {
        /* 19-digit value that fits long long but is huge */
        int rc = parse_timespec("9223372036854775807", &result);
        if (sizeof(time_t) >= 8) {
            /* On 64-bit: this is LLONG_MAX = TIME_T_MAX for signed 64-bit.
             * val == TIME_T_MAX should be accepted (val > TIME_T_MAX is the check). */
            CHECK("V3: LLONG_MAX epoch on 64-bit",
                  rc == 0 && result == (time_t)9223372036854775807LL);
        } else {
            CHECK("V3: LLONG_MAX epoch rejected (32-bit time_t)", rc == -1);
        }
    }

    /*
     * V4: long long to time_t cast overflow (relative path).
     * Construct a relative spec whose multiplied offset exceeds TIME_T_MAX
     * but doesn't overflow long long.
     */
    if (sizeof(time_t) < 8) {
        /* On 32-bit time_t, max is ~2^31-1 = 2147483647.
         * "2147483648s" => offset 2147483648 > TIME_T_MAX => rejected */
        int rc = parse_timespec("2147483648s", &result);
        CHECK("V4: offset > 32-bit TIME_T_MAX rejected", rc == -1);
    }
    {
        /* On any platform: a very large day count that when multiplied
         * would overflow. "999999999999999d" — val * 86400 overflows long long. */
        int rc = parse_timespec("999999999999999d", &result);
        CHECK("V4: massive day count overflow rejected", rc == -1);
    }

    /*
     * V8: time(NULL) failure.
     * Cannot easily unit-test without intercepting the time() call.
     * The fix is verified by code inspection: the check is present.
     * We document this gap explicitly.
     */

    /* --- Adversarial tests for HARDENING violations --- */

    printf("\n   --- Adversarial: HARDENING violations ---\n");

    /*
     * V2: Zero offset is degenerate and should be rejected.
     * Test all suffix variants.
     */
    {
        int rc = parse_timespec("0s", &result);
        CHECK("V2: '0s' rejected as degenerate", rc == -1);
    }
    {
        int rc = parse_timespec("0m", &result);
        CHECK("V2: '0m' rejected as degenerate", rc == -1);
    }
    {
        int rc = parse_timespec("0h", &result);
        CHECK("V2: '0h' rejected as degenerate", rc == -1);
    }
    {
        int rc = parse_timespec("0d", &result);
        CHECK("V2: '0d' rejected as degenerate", rc == -1);
    }

    /*
     * V5: Pre-epoch ISO 8601 dates should be rejected.
     * mktime returns (time_t)-1 for 1969-12-31T23:59:59 (if timezone
     * allows it). The fix disambiguates error from valid pre-epoch.
     * Either way, t <= 0 rejects pre-epoch dates.
     */
    {
        /* Use 1960 — unambiguously pre-epoch regardless of timezone */
        int rc = parse_timespec("1960-01-01T00:00:00", &result);
        CHECK("V5: pre-epoch ISO 8601 rejected", rc == -1);
    }
    {
        /* Epoch exactly: 1970-01-01T00:00:00 => t=0 in UTC, but mktime
         * uses local time, so this may be non-zero. If local is west of UTC,
         * t would be positive. If east or UTC, t=0 or negative => rejected.
         * We just verify it doesn't crash and returns a sensible answer. */
        int rc = parse_timespec("1970-01-01T00:00:00", &result);
        /* Accept either result: rejected (t<=0) or accepted (t>0, timezone) */
        CHECK("V5: epoch-boundary ISO 8601 handled",
              rc == -1 || (rc == 0 && result > 0));
    }

    /*
     * V9: Empty string now triggers ASSERT_MSG (abort).
     * We cannot call parse_timespec("", ...) without crashing.
     * Verified by code inspection: ASSERT_MSG(len > 0, ...) is present.
     */

    /*
     * V10: *out unchanged on error paths.
     * Set result to a sentinel, call with invalid spec, verify unchanged.
     */
    {
        time_t sentinel = 42;
        result = sentinel;
        int rc = parse_timespec("not-a-time", &result);
        CHECK("V10: *out unchanged on error",
              rc == -1 && result == sentinel);
    }
    {
        time_t sentinel = 99;
        result = sentinel;
        int rc = parse_timespec("-5m", &result);
        CHECK("V10: *out unchanged on negative",
              rc == -1 && result == sentinel);
    }
    {
        time_t sentinel = 77;
        result = sentinel;
        int rc = parse_timespec("abc", &result);
        CHECK("V10: *out unchanged on garbage",
              rc == -1 && result == sentinel);
    }

    /* --- Additional adversarial boundary inputs --- */

    printf("\n   --- Additional adversarial inputs ---\n");

    /* Single character that is a suffix */
    {
        int rc = parse_timespec("s", &result);
        CHECK("single 's' rejected", rc == -1);
    }
    {
        int rc = parse_timespec("m", &result);
        CHECK("single 'm' rejected", rc == -1);
    }

    /* Leading zeros in relative */
    {
        int rc = parse_timespec("007s", &result);
        CHECK("leading zeros relative parses", rc == 0);
        CHECK("leading zeros value correct",
              result >= now - 9 && result <= now - 5);
    }

    /* Very short epoch (< 10 digits, all digits) — should fall through */
    {
        int rc = parse_timespec("123456789", &result);
        CHECK("9-digit epoch rejected (< 10 digits)", rc == -1);
    }

    /* Exactly 10-digit epoch at minimum */
    {
        int rc = parse_timespec("1000000000", &result);
        CHECK("10-digit minimum epoch parses", rc == 0);
        CHECK("10-digit minimum epoch value",
              result == (time_t)1000000000LL);
    }

    /* Trailing garbage after ISO 8601 */
    {
        int rc = parse_timespec("2025-01-01T00:00:00Z", &result);
        CHECK("ISO 8601 with trailing Z rejected", rc == -1);
    }

    /* Partial ISO 8601 */
    {
        int rc = parse_timespec("2025-01-01", &result);
        CHECK("partial ISO 8601 (date only) rejected", rc == -1);
    }

    /* Mixed digits and suffix that doesn't match format */
    {
        int rc = parse_timespec("12x", &result);
        CHECK("invalid suffix 'x' rejected", rc == -1);
    }

    /* Whitespace around valid input */
    {
        int rc = parse_timespec(" 30s", &result);
        CHECK("leading space rejected", rc == -1);
    }
    {
        int rc = parse_timespec("30s ", &result);
        CHECK("trailing space rejected", rc == -1);
    }

    printf("\n%d/%d passed\n", tests - fails, tests);
    return fails;
}
