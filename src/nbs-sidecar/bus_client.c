/*
 * bus_client.c — Bus operations via fork+exec of nbs-bus binary.
 *
 * Ported from the bash implementation in bin/nbs-claude:
 *   check_bus_events       → bus_client_check
 *   check_interrupt_events → bus_client_check_typed (type="chat-interrupt")
 *   check_mention_events   → bus_client_check_typed (type="chat-mention")
 *
 * All external command execution goes through exec_capture and
 * exec_fire_and_forget (exec_util.h). No direct fork/exec here.
 *
 * Invariants:
 *   - All string construction uses snprintf with bounded buffers
 *   - bus_client_check_typed acks every matching event before returning
 *   - Priority extraction handles malformed output gracefully (defaults to "none")
 */

#include "bus_client.h"
#include "exec_util.h"
#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <string.h>

#define CMD_BUF_SIZE 8192

int bus_client_check(const char *bus_dir, int *event_count,
                     char *max_priority, size_t mp_size,
                     char *summary, size_t sum_size)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_check: bus_dir is NULL");
    ASSERT_MSG(event_count != NULL, "bus_client_check: event_count is NULL");
    ASSERT_MSG(max_priority != NULL, "bus_client_check: max_priority is NULL");
    ASSERT_MSG(mp_size > 0, "bus_client_check: mp_size is 0");
    ASSERT_MSG(summary != NULL, "bus_client_check: summary is NULL");
    ASSERT_MSG(sum_size > 0, "bus_client_check: sum_size is 0");

    const char *argv[] = {"nbs-bus", "check", bus_dir, NULL};
    char buf[CMD_BUF_SIZE];

    int rc = exec_capture(argv, buf, sizeof(buf));
    if (rc != 0 || buf[0] == '\0') {
        *event_count = 0;
        snprintf(max_priority, mp_size, "none");
        summary[0] = '\0';
        return 1; /* empty */
    }

    /* Count lines in buf */
    int count = 0;
    for (const char *p = buf; *p != '\0'; p++) {
        if (*p == '\n')
            count++;
    }
    /* If buf is non-empty and doesn't end with newline, count the last line */
    if (buf[0] != '\0') {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] != '\n')
            count++;
    }
    *event_count = count;

    /*
     * Extract priority from first line: [priority] filename (age)
     * Find '[', then ']', copy substring between them.
     */
    const char *open = strchr(buf, '[');
    const char *close = (open != NULL) ? strchr(open, ']') : NULL;
    if (open != NULL && close != NULL && close > open + 1) {
        size_t plen = (size_t)(close - open - 1);
        if (plen >= mp_size)
            plen = mp_size - 1;
        memcpy(max_priority, open + 1, plen);
        max_priority[plen] = '\0';
    } else {
        snprintf(max_priority, mp_size, "none");
    }

    /* Build summary */
    snprintf(summary, sum_size, "%d event(s) in %s", count, bus_dir);

    return 0;
}

int bus_client_read(const char *bus_dir, const char *event_file,
                    char *payload, size_t payload_size)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_read: bus_dir is NULL");
    ASSERT_MSG(event_file != NULL, "bus_client_read: event_file is NULL");
    ASSERT_MSG(payload != NULL, "bus_client_read: payload is NULL");
    ASSERT_MSG(payload_size > 0, "bus_client_read: payload_size is 0");

    const char *argv[] = {"nbs-bus", "read", bus_dir, event_file, NULL};

    int rc = exec_capture(argv, payload, payload_size);
    if (rc < 0)
        return -1;

    return 0;
}

int bus_client_ack(const char *bus_dir, const char *event_file)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_ack: bus_dir is NULL");
    ASSERT_MSG(event_file != NULL, "bus_client_ack: event_file is NULL");

    const char *argv[] = {"nbs-bus", "ack", bus_dir, event_file, NULL};

    int rc = exec_fire_and_forget(argv);
    if (rc < 0)
        return -1;

    return 0;
}

int bus_client_publish(const char *bus_dir, const char *source,
                       const char *type, const char *priority,
                       const char *payload)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_publish: bus_dir is NULL");
    ASSERT_MSG(source != NULL, "bus_client_publish: source is NULL");
    ASSERT_MSG(type != NULL, "bus_client_publish: type is NULL");
    ASSERT_MSG(priority != NULL, "bus_client_publish: priority is NULL");
    ASSERT_MSG(payload != NULL, "bus_client_publish: payload is NULL");

    const char *argv[] = {"nbs-bus", "publish", bus_dir,
                          source, type, priority, payload, NULL};

    int rc = exec_fire_and_forget(argv);
    if (rc < 0)
        return -1;

    return 0;
}

int bus_client_check_typed(const char *bus_dir, const char *event_type,
                            const char *target_handle,
                            char *payload_out, size_t payload_size)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_check_typed: bus_dir is NULL");
    ASSERT_MSG(event_type != NULL, "bus_client_check_typed: event_type is NULL");
    ASSERT_MSG(target_handle != NULL, "bus_client_check_typed: target_handle is NULL");
    ASSERT_MSG(payload_out != NULL, "bus_client_check_typed: payload_out is NULL");
    ASSERT_MSG(payload_size > 0, "bus_client_check_typed: payload_size is 0");

    /* Run nbs-bus check to get the event listing */
    const char *argv[] = {"nbs-bus", "check", bus_dir, NULL};
    char buf[CMD_BUF_SIZE];

    int rc = exec_capture(argv, buf, sizeof(buf));
    if (rc != 0 || buf[0] == '\0')
        return 1; /* no events */

    /* Build the @handle string for matching */
    char at_handle[256];
    snprintf(at_handle, sizeof(at_handle), "@%s", target_handle);

    /*
     * Parse output line by line. Each line is:
     *   [priority] filename (age)
     * Filter for lines containing event_type, extract filename,
     * read payload, check for @handle, ack if match.
     */
    char *line_start = buf;
    while (line_start != NULL && *line_start != '\0') {
        /* Find end of current line */
        char *line_end = strchr(line_start, '\n');
        if (line_end != NULL)
            *line_end = '\0';

        /* Check if this line contains the event type */
        if (strstr(line_start, event_type) != NULL) {
            /*
             * Extract filename: second space-delimited field.
             * Line format: [priority] filename (age)
             * Skip past first space to get to filename.
             */
            const char *p = strchr(line_start, ' ');
            if (p != NULL) {
                /* Skip leading spaces */
                while (*p == ' ')
                    p++;

                /* p now points to filename; find its end */
                const char *fname_start = p;
                const char *fname_end = strchr(p, ' ');
                size_t fname_len;
                if (fname_end != NULL) {
                    fname_len = (size_t)(fname_end - fname_start);
                } else {
                    fname_len = strlen(fname_start);
                }

                char event_file[1024];
                if (fname_len >= sizeof(event_file))
                    fname_len = sizeof(event_file) - 1;
                memcpy(event_file, fname_start, fname_len);
                event_file[fname_len] = '\0';

                /* Read the event payload */
                char payload_buf[CMD_BUF_SIZE];
                if (bus_client_read(bus_dir, event_file,
                                    payload_buf, sizeof(payload_buf)) == 0) {
                    /* Check if @handle appears in the payload */
                    if (strstr(payload_buf, at_handle) != NULL) {
                        /* Ack the event */
                        bus_client_ack(bus_dir, event_file);

                        /* Copy payload to output */
                        snprintf(payload_out, payload_size, "%s", payload_buf);
                        return 0; /* match found */
                    }
                }
            }
        }

        /* Advance to next line */
        if (line_end != NULL)
            line_start = line_end + 1;
        else
            break;
    }

    return 1; /* no matching event */
}
