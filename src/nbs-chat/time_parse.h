/*
 * time_parse.h — Parse human-friendly time specifications.
 *
 * Supports three formats:
 *   - Relative: "30s", "5m", "2h", "1d" (ago from now)
 *   - Epoch: "1771834287" (Unix timestamp, ≥10 digits)
 *   - ISO 8601: "2026-02-23T00:11:27" (local time)
 */

#ifndef NBS_TIME_PARSE_H
#define NBS_TIME_PARSE_H

#include <time.h>

/*
 * parse_timespec — Convert a time specification string to epoch seconds.
 *
 * Preconditions:
 *   - spec != NULL, NUL-terminated, non-empty
 *   - out != NULL
 *
 * Postconditions:
 *   - Returns 0 on success: *out contains a positive epoch value
 *   - Returns -1 on parse error: *out is undefined
 *
 * Format detection:
 *   - If spec ends with 's', 'm', 'h', or 'd' and the rest is digits:
 *     relative time (subtracted from current time)
 *   - If spec is all digits and length ≥ 10: epoch seconds
 *   - Otherwise: try ISO 8601 parse (YYYY-MM-DDTHH:MM:SS)
 */
int parse_timespec(const char *spec, time_t *out);

#endif /* NBS_TIME_PARSE_H */
