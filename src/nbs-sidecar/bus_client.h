/*
 * bus_client.h — Bus operations via fork+exec of nbs-bus binary.
 */

#ifndef NBS_BUS_CLIENT_H
#define NBS_BUS_CLIENT_H

#include <stddef.h>

/*
 * bus_client_check — Run `nbs-bus check <dir>`, count pending events.
 *
 * Preconditions:
 *   - bus_dir != NULL
 *   - event_count != NULL
 *   - max_priority != NULL, mp_size > 0
 *   - summary != NULL, sum_size > 0
 *
 * Postconditions:
 *   - *event_count set to number of pending events
 *   - max_priority filled with highest priority string (or "none" if empty/error)
 *   - summary filled with human-readable summary (or empty if error)
 *   - Returns: 0 = events found, 1 = empty, -1 = error
 */
int bus_client_check(const char *bus_dir, int *event_count,
                     char *max_priority, size_t mp_size,
                     char *summary, size_t sum_size);

/*
 * bus_client_read — Run `nbs-bus read <dir> <file>`, capture payload.
 *
 * Preconditions:
 *   - bus_dir != NULL
 *   - event_file != NULL
 *   - payload != NULL, payload_size > 0
 *
 * Postconditions:
 *   - On success (return 0): payload contains event data (NUL-terminated)
 *   - On error (return -1): payload contents undefined
 */
int bus_client_read(const char *bus_dir, const char *event_file,
                    char *payload, size_t payload_size);

/*
 * bus_client_ack — Run `nbs-bus ack <dir> <file>`.
 *
 * Preconditions:
 *   - bus_dir != NULL
 *   - event_file != NULL
 *
 * Postconditions:
 *   - Returns 0 on success, -1 on error
 */
int bus_client_ack(const char *bus_dir, const char *event_file);

/*
 * bus_client_publish — Run `nbs-bus publish <dir> <source> <type> <priority> <payload>`.
 *
 * Preconditions:
 *   - bus_dir != NULL
 *   - source != NULL
 *   - type != NULL
 *   - priority != NULL
 *   - payload != NULL
 *
 * Postconditions:
 *   - Returns 0 on success, -1 on error
 */
int bus_client_publish(const char *bus_dir, const char *source,
                       const char *type, const char *priority,
                       const char *payload);

/*
 * bus_client_check_typed — Check for events of a specific type targeting
 * a specific handle (via @handle in payload). Acks matching events.
 *
 * Used for interrupt and mention detection.
 *
 * Preconditions:
 *   - bus_dir != NULL
 *   - event_type != NULL
 *   - target_handle != NULL
 *   - payload_out != NULL, payload_size > 0
 *
 * Postconditions:
 *   - If matching event found: acked, payload_out populated, returns 0
 *   - If no match: returns 1
 *   - On error: returns -1
 */
int bus_client_check_typed(const char *bus_dir, const char *event_type,
                            const char *target_handle,
                            char *payload_out, size_t payload_size);

#endif /* NBS_BUS_CLIENT_H */
