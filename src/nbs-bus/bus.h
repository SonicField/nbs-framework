/*
 * bus.h — NBS Bus: event-driven coordination queue
 *
 * File-based event queue in .nbs/events/. Each event is a single YAML file:
 *
 *   <unix-timestamp-us>-<source-handle>-<event-type>-<pid>.event
 *
 * Content:
 *   source: <handle>
 *   type: <event-type>
 *   priority: <critical|high|normal|low>
 *   timestamp: <ISO 8601>
 *   dedup-key: <source>:<type>
 *   payload: |
 *     <free-form text>
 *
 * Events flow: publish -> queue -> check -> read -> ack (move to processed/)
 *
 * Invariants (enforced by ASSERT_MSG):
 *   - priority is one of: critical(0), high(1), normal(2), low(3)
 *   - event filenames match the naming convention
 *   - source and type are non-empty, no whitespace
 *   - publish creates files atomically (write-temp, rename)
 *   - ack moves files atomically (rename)
 *   - prune only deletes from processed/
 */

#ifndef NBS_BUS_H
#define NBS_BUS_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/*
 * ASSERT_MSG — Assert with context-aware message.
 *
 * Always fires (not gated by NDEBUG) — asserts are executable specifications.
 */
#define ASSERT_MSG(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERT FAILED %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        abort(); \
    } \
} while(0)

/* Limits */
#define BUS_MAX_PATH       4096
#define BUS_MAX_HANDLE      128
#define BUS_MAX_TYPE        128
#define BUS_MAX_PAYLOAD   16384
#define BUS_MAX_EVENTS     4096
#define BUS_MAX_FILENAME    512
#define BUS_MAX_FULLPATH   (BUS_MAX_PATH + BUS_MAX_FILENAME + 64)

/* Priority levels */
#define BUS_PRIORITY_CRITICAL  0
#define BUS_PRIORITY_HIGH      1
#define BUS_PRIORITY_NORMAL    2
#define BUS_PRIORITY_LOW       3

/* Exit codes */
#define BUS_EXIT_OK            0
#define BUS_EXIT_ERROR         1
#define BUS_EXIT_DIR_NOT_FOUND 2
#define BUS_EXIT_NOT_FOUND     3
#define BUS_EXIT_BAD_ARGS      4
#define BUS_EXIT_DEDUP         5

/* Parsed event (in-memory representation) */
typedef struct {
    char filename[BUS_MAX_FILENAME]; /* just the filename, not the full path */
    char source[BUS_MAX_HANDLE];
    char type[BUS_MAX_TYPE];
    int  priority;                   /* 0=critical, 1=high, 2=normal, 3=low */
    long long timestamp_us;          /* unix timestamp in microseconds */
} bus_event_t;

/*
 * bus_priority_from_str — Parse a priority string to integer.
 *
 * Preconditions:
 *   - s != NULL
 *
 * Returns 0-3 on success, -1 if unrecognised.
 */
int bus_priority_from_str(const char *s);

/*
 * bus_priority_to_str — Convert a priority integer to string.
 *
 * Preconditions:
 *   - p in [0, 3] (asserted)
 *
 * Returns a static string: "critical", "high", "normal", or "low".
 */
const char *bus_priority_to_str(int p);

/*
 * bus_publish — Create an event file atomically.
 *
 * Preconditions:
 *   - events_dir != NULL, points to an existing .nbs/events/ directory
 *   - source != NULL, non-empty, no whitespace
 *   - type != NULL, non-empty, no whitespace
 *   - priority in [0, 3]
 *   - payload may be NULL (omitted from event file)
 *
 * Postconditions (on success, return 0):
 *   - A new .event file exists in events_dir
 *   - File was created atomically (write-temp, rename)
 *   - Filename printed to stdout
 *
 * Returns 0 on success, -1 on error.
 */
int bus_publish(const char *events_dir, const char *source, const char *type,
                int priority, const char *payload);

/*
 * bus_publish_dedup — Publish with deduplication.
 *
 * Before creating the event, scans pending events for a matching dedup-key
 * (source:type) with a timestamp within dedup_window_us of now.  If a
 * match is found, the event is dropped (not published).
 *
 * Preconditions:
 *   - Same as bus_publish
 *   - dedup_window_us > 0
 *
 * Postconditions:
 *   - If no duplicate: same as bus_publish (return 0)
 *   - If duplicate found: no file created, return BUS_EXIT_DEDUP (5)
 *
 * Returns 0 on success, BUS_EXIT_DEDUP if deduplicated, -1 on error.
 */
int bus_publish_dedup(const char *events_dir, const char *source,
                      const char *type, int priority, const char *payload,
                      long long dedup_window_us);

/*
 * bus_check — List pending events sorted by priority then timestamp.
 *
 * Preconditions:
 *   - events_dir != NULL, points to an existing .nbs/events/ directory
 *   - handle may be NULL (no filtering)
 *
 * Postconditions:
 *   - Events printed to stdout: [priority] filename
 *   - Output is sorted: lowest priority number first, then oldest timestamp first
 *
 * Returns 0 on success (even if no events), -1 on error.
 */
int bus_check(const char *events_dir, const char *handle);

/*
 * bus_read — Read and display a single event file.
 *
 * Preconditions:
 *   - events_dir != NULL
 *   - event_file != NULL (filename, not full path)
 *
 * Postconditions:
 *   - Event content printed to stdout
 *
 * Returns 0 on success, -1 if file not found or read error.
 */
int bus_read(const char *events_dir, const char *event_file);

/*
 * bus_ack — Acknowledge an event by moving it to processed/.
 *
 * Preconditions:
 *   - events_dir != NULL
 *   - event_file != NULL (filename, not full path)
 *
 * Postconditions (on success, return 0):
 *   - Event file moved from events_dir to events_dir/processed/
 *
 * Returns 0 on success, -1 on error.
 */
int bus_ack(const char *events_dir, const char *event_file);

/*
 * bus_ack_all — Acknowledge all pending events.
 *
 * Preconditions:
 *   - events_dir != NULL
 *   - handle may be NULL (ack all events)
 *
 * Postconditions:
 *   - All matching .event files moved to processed/
 *   - Count of acknowledged events printed to stdout
 *
 * Returns 0 on success, -1 on error.
 */
int bus_ack_all(const char *events_dir, const char *handle);

/*
 * bus_prune — Delete oldest processed events when size limit exceeded.
 *
 * Preconditions:
 *   - events_dir != NULL
 *   - max_bytes > 0
 *
 * Postconditions:
 *   - Total size of processed/ directory <= max_bytes
 *   - Oldest files deleted first
 *   - Count of pruned events printed to stdout
 *
 * Returns 0 on success, -1 on error.
 */
int bus_prune(const char *events_dir, long long max_bytes);

/*
 * bus_status — Print summary of bus state.
 *
 * Preconditions:
 *   - events_dir != NULL
 *
 * Postconditions:
 *   - Summary printed to stdout: pending count by priority,
 *     processed count, oldest pending, total size
 *
 * Returns 0 on success, -1 on error.
 */
int bus_status(const char *events_dir);

#endif /* NBS_BUS_H */
