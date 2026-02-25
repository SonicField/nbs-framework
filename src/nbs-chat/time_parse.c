/*
 * time_parse.c — Parse human-friendly time specifications.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700  /* strptime */
#endif

#include "time_parse.h"
#include "../nbs-common/nbs_assert.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int parse_timespec(const char *spec, time_t *out) {
    ASSERT_MSG(spec != NULL, "parse_timespec: spec is NULL");
    ASSERT_MSG(out != NULL, "parse_timespec: out is NULL");

    size_t len = strlen(spec);
    if (len == 0) return -1;

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
            if (endptr != spec + len - 1 || val < 0) return -1;

            long long multiplier = 1;
            switch (suffix) {
                case 's': multiplier = 1; break;
                case 'm': multiplier = 60; break;
                case 'h': multiplier = 3600; break;
                case 'd': multiplier = 86400; break;
            }

            /* Overflow check */
            if (val > 0 && multiplier > LLONG_MAX / val) return -1;
            long long offset = val * multiplier;

            time_t now = time(NULL);
            if (now < offset) return -1;  /* Would go negative */

            *out = now - (time_t)offset;
            return 0;
        }
    }

    /* Epoch: all digits, length ≥ 10 */
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
            long long val = strtoll(spec, &endptr, 10);
            if (*endptr != '\0' || val <= 0) return -1;
            *out = (time_t)val;
            return 0;
        }
    }

    /* ISO 8601: YYYY-MM-DDTHH:MM:SS */
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_isdst = -1;  /* Let mktime determine DST */
    char *rest = strptime(spec, "%Y-%m-%dT%H:%M:%S", &tm);
    if (rest != NULL && *rest == '\0') {
        time_t t = mktime(&tm);  /* Interprets as local time */
        if (t == (time_t)-1) return -1;
        *out = t;
        return 0;
    }

    return -1;
}
