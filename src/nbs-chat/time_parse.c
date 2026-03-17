/*
 * time_parse.c — Parse human-friendly time specifications.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700  /* strptime */
#endif

#include "time_parse.h"
#include "../nbs-common/nbs_assert.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * TIME_T_MAX — portable upper bound for time_t.
 * If time_t is unsigned, (time_t)-1 is the max.
 * If time_t is signed (the common case), compute the max for its width.
 */
#define TIME_T_MAX ( \
    (time_t)-1 > 0 \
        ? (long long)(time_t)-1 \
        : (long long)(((unsigned long long)1 << (sizeof(time_t) * 8 - 1)) - 1) \
)

int parse_timespec(const char *spec, time_t *out) {
    /* V6: Precondition assertions with context values */
    ASSERT_MSG(spec != NULL,
               "parse_timespec: spec is NULL, out=%p", (void *)out);
    ASSERT_MSG(out != NULL,
               "parse_timespec: out is NULL, spec=%p", (const void *)spec);

    size_t len = strlen(spec);

    /* V9: Empty string is a precondition violation, not a normal error */
    ASSERT_MSG(len > 0,
               "parse_timespec: spec is empty string, caller must not pass empty");

    /*
     * V10: Use a local result variable. Only write *out immediately
     * before return 0 to prevent partial writes on error paths.
     */
    time_t result;

    /* Relative: digits followed by s/m/h/d */
    char suffix = spec[len - 1];
    if (len >= 2 && (suffix == 's' || suffix == 'm' || suffix == 'h' || suffix == 'd')) {
        /* Check all preceding characters are digits */
        int all_digits = 1;
        for (size_t i = 0; i < len - 1; i++) {
            if (!isdigit((unsigned char)spec[i])) {
                all_digits = 0;
                break;
            }
        }
        if (all_digits) {
            char *endptr;
            long long val = strtoll(spec, &endptr, 10);
            if (endptr != spec + len - 1 || val < 0) {
                fprintf(stderr, "parse_timespec: relative parse failed for '%s'\n", spec);
                return -1;
            }

            /* V2: Reject zero offset — "0s" is degenerate */
            if (val == 0) {
                fprintf(stderr, "parse_timespec: zero offset is degenerate: '%s'\n", spec);
                return -1;
            }

            long long multiplier = 1;
            switch (suffix) {
                case 's': multiplier = 1; break;
                case 'm': multiplier = 60; break;
                case 'h': multiplier = 3600; break;
                case 'd': multiplier = 86400; break;
            }

            /* Overflow check */
            if (val > 0 && multiplier > LLONG_MAX / val) {
                fprintf(stderr, "parse_timespec: multiplication overflow for '%s' "
                        "(val=%lld, multiplier=%lld)\n", spec, val, multiplier);
                return -1;
            }
            long long offset = val * multiplier;

            /* V4: Range check before casting offset to time_t */
            if (offset > TIME_T_MAX) {
                fprintf(stderr, "parse_timespec: offset %lld exceeds time_t range for '%s'\n",
                        offset, spec);
                return -1;
            }

            /* V8: time(NULL) can fail — check for (time_t)-1 */
            time_t now = time(NULL);
            if (now == (time_t)-1) {
                fprintf(stderr, "parse_timespec: time(NULL) failed for '%s'\n", spec);
                return -1;
            }

            if (now < (time_t)offset) {
                fprintf(stderr, "parse_timespec: offset %lld exceeds current time for '%s'\n",
                        offset, spec);
                return -1;  /* Would go negative */
            }

            result = now - (time_t)offset;

            /* V1: Postcondition — result must be a positive epoch */
            ASSERT_MSG(result > 0,
                       "parse_timespec postcondition: relative result must be "
                       "positive epoch, got %lld (now=%lld, offset=%lld)",
                       (long long)result, (long long)now, offset);

            *out = result;
            return 0;
        }
    }

    /* Epoch: all digits, length >= 10 */
    if (len >= 10) {
        int all_digits = 1;
        for (size_t i = 0; i < len; i++) {
            if (!isdigit((unsigned char)spec[i])) {
                all_digits = 0;
                break;
            }
        }
        if (all_digits) {
            char *endptr;
            errno = 0;
            long long val = strtoll(spec, &endptr, 10);
            if (*endptr != '\0' || val <= 0 || errno == ERANGE) {
                fprintf(stderr, "parse_timespec: epoch parse failed for '%s' "
                        "(errno=%d)\n", spec, errno);
                return -1;
            }

            /* V3: Range check before casting val to time_t */
            if (val > TIME_T_MAX) {
                fprintf(stderr, "parse_timespec: epoch %lld exceeds time_t range for '%s'\n",
                        val, spec);
                return -1;
            }

            result = (time_t)val;

            /* V1: Postcondition — result must be a positive epoch */
            ASSERT_MSG(result > 0,
                       "parse_timespec postcondition: epoch result must be "
                       "positive, got %lld from spec='%s'",
                       (long long)result, spec);

            *out = result;
            return 0;
        }
    }

    /* ISO 8601: YYYY-MM-DDTHH:MM:SS */
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_isdst = -1;  /* Let mktime determine DST */
    char *rest = strptime(spec, "%Y-%m-%dT%H:%M:%S", &tm);
    if (rest != NULL && *rest == '\0') {
        /* V5: Disambiguate mktime error from valid (time_t)-1 */
        errno = 0;
        time_t t = mktime(&tm);  /* Interprets as local time */
        if (t == (time_t)-1 && errno != 0) {
            fprintf(stderr, "parse_timespec: mktime failed for '%s' (errno=%d)\n",
                    spec, errno);
            return -1;
        }
        if (t <= 0) {
            fprintf(stderr, "parse_timespec: pre-epoch date rejected for '%s' "
                    "(mktime returned %lld)\n", spec, (long long)t);
            return -1;  /* Reject pre-epoch dates */
        }

        result = t;

        /* V1: Postcondition — result must be a positive epoch */
        ASSERT_MSG(result > 0,
                   "parse_timespec postcondition: ISO 8601 result must be "
                   "positive, got %lld from spec='%s'",
                   (long long)result, spec);

        *out = result;
        return 0;
    }

    fprintf(stderr, "parse_timespec: unrecognised format '%s'\n", spec);
    return -1;
}
