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
 *   - bus_client_check_typed does NOT ack — caller must use bus_client_ack_event
 *   - bus_client_ack_event validates filename (no path traversal)
 *   - Priority extraction handles malformed output gracefully (defaults to "none")
 *
 * Threading:
 *   - resolve_nbs_bus() uses pthread_once for thread-safe one-time initialisation.
 *   - All other functions are stateless and safe to call from any thread.
 */

#include "bus_client.h"
#include "exec_util.h"
#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define CMD_BUF_SIZE 65536

/*
 * Cached absolute path to the nbs-bus binary.
 * Resolved once via pthread_once — the binary is expected
 * to be in the same directory as nbs-sidecar (sibling binary).
 * If resolution fails, falls back to "nbs-bus" (PATH search via execvp).
 */
#define BUS_PATH_LEN 4096
static char nbs_bus_path[BUS_PATH_LEN] = "";
static pthread_once_t nbs_bus_once = PTHREAD_ONCE_INIT;

static void resolve_nbs_bus_once(void)
{
    char self[BUS_PATH_LEN];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len <= 0)
        return;
    self[len] = '\0';

    char *slash = strrchr(self, '/');
    if (!slash)
        return;

    size_t dir_len = (size_t)(slash - self);
    if (dir_len + sizeof("/nbs-bus") > sizeof(nbs_bus_path))
        return;

    memcpy(nbs_bus_path, self, dir_len);
    memcpy(nbs_bus_path + dir_len, "/nbs-bus", sizeof("/nbs-bus"));

    if (access(nbs_bus_path, X_OK) != 0) {
        nbs_bus_path[0] = '\0';
        return;
    }
}

static const char *resolve_nbs_bus(void)
{
    pthread_once(&nbs_bus_once, resolve_nbs_bus_once);
    if (nbs_bus_path[0] != '\0')
        return nbs_bus_path;
    return "nbs-bus";
}

int bus_client_check(const char *bus_dir, int *event_count,
                     char *max_priority, size_t mp_size,
                     char *summary, size_t sum_size)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_check: bus_dir is NULL");
    ASSERT_MSG(bus_dir[0] != '\0', "bus_client_check: bus_dir is empty");
    ASSERT_MSG(event_count != NULL, "bus_client_check: event_count is NULL");
    ASSERT_MSG(max_priority != NULL, "bus_client_check: max_priority is NULL");
    ASSERT_MSG(mp_size > 0, "bus_client_check: mp_size is 0");
    ASSERT_MSG(summary != NULL, "bus_client_check: summary is NULL");
    ASSERT_MSG(sum_size > 0, "bus_client_check: sum_size is 0");

    const char *argv[] = {resolve_nbs_bus(), "check", bus_dir, NULL};
    char buf[CMD_BUF_SIZE];

    int rc = exec_capture(argv, buf, sizeof(buf));
    if (rc < 0) {
        /* exec failure (binary not found, fork failed, etc.) */
        fprintf(stderr, "bus_client_check: exec_capture failed for '%s'\n",
                argv[0]);
        *event_count = 0;
        snprintf(max_priority, mp_size, "none");
        summary[0] = '\0';
        return -1;
    }
    if (rc != 0 || buf[0] == '\0') {
        *event_count = 0;
        snprintf(max_priority, mp_size, "none");
        summary[0] = '\0';
        return 1; /* empty */
    }

    /* Warn if output may have been truncated */
    if (strlen(buf) >= sizeof(buf) - 1) {
        fprintf(stderr, "bus_client_check: output may be truncated "
                "(%zu bytes, buffer %zu)\n", strlen(buf), sizeof(buf));
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

    /* Postconditions: when returning 0 (events found), outputs must be valid */
    ASSERT_MSG(*event_count > 0,
               "bus_client_check: returning 0 but event_count is %d",
               *event_count);
    ASSERT_MSG(max_priority[0] != '\0',
               "bus_client_check: returning 0 but max_priority is empty");
    ASSERT_MSG(summary[0] != '\0',
               "bus_client_check: returning 0 but summary is empty");

    return 0;
}

int bus_client_read(const char *bus_dir, const char *event_file,
                    char *payload, size_t payload_size)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_read: bus_dir is NULL");
    ASSERT_MSG(bus_dir[0] != '\0', "bus_client_read: bus_dir is empty");
    ASSERT_MSG(event_file != NULL, "bus_client_read: event_file is NULL");
    ASSERT_MSG(event_file[0] != '\0', "bus_client_read: event_file is empty");
    ASSERT_MSG(payload != NULL, "bus_client_read: payload is NULL");
    ASSERT_MSG(payload_size > 0, "bus_client_read: payload_size is 0");

    const char *argv[] = {resolve_nbs_bus(), "read", bus_dir, event_file, NULL};

    int rc = exec_capture(argv, payload, payload_size);
    if (rc < 0)
        return -1;

    /* Empty payload is valid — bus_publish allows empty payloads.
     * Callers (check_typed) will not match @handle in empty string
     * and will skip the event. Log for observability but do not abort. */
    if (payload[0] == '\0') {
        fprintf(stderr, "bus_client_read: payload is empty for event '%s' "
                "(valid but unusual)\n", event_file);
    }

    return 0;
}

int bus_client_ack(const char *bus_dir, const char *event_file)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_ack: bus_dir is NULL");
    ASSERT_MSG(bus_dir[0] != '\0', "bus_client_ack: bus_dir is empty");
    ASSERT_MSG(event_file != NULL, "bus_client_ack: event_file is NULL");
    ASSERT_MSG(event_file[0] != '\0', "bus_client_ack: event_file is empty");

    const char *argv[] = {resolve_nbs_bus(), "ack", bus_dir, event_file, NULL};

    int rc = exec_fire_and_forget(argv);
    if (rc != 0)
        return -1;

    return 0;
}

int bus_client_publish(const char *bus_dir, const char *source,
                       const char *type, const char *priority,
                       const char *payload)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_publish: bus_dir is NULL");
    ASSERT_MSG(bus_dir[0] != '\0', "bus_client_publish: bus_dir is empty");
    ASSERT_MSG(source != NULL, "bus_client_publish: source is NULL");
    ASSERT_MSG(source[0] != '\0', "bus_client_publish: source is empty");
    ASSERT_MSG(type != NULL, "bus_client_publish: type is NULL");
    ASSERT_MSG(type[0] != '\0', "bus_client_publish: type is empty");
    ASSERT_MSG(priority != NULL, "bus_client_publish: priority is NULL");
    ASSERT_MSG(priority[0] != '\0', "bus_client_publish: priority is empty");
    ASSERT_MSG(payload != NULL, "bus_client_publish: payload is NULL");
    /* Empty payload is permitted — nbs-bus accepts zero-length payloads */

    const char *argv[] = {resolve_nbs_bus(), "publish", bus_dir,
                          source, type, priority, payload, NULL};

    int rc = exec_fire_and_forget(argv);
    if (rc < 0)
        return -1;

    return 0;
}

int bus_client_check_typed(const char *bus_dir, const char *event_type,
                            const char *target_handle,
                            char *payload_out, size_t payload_size,
                            char *event_file_out, size_t event_file_size)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_check_typed: bus_dir is NULL");
    ASSERT_MSG(bus_dir[0] != '\0', "bus_client_check_typed: bus_dir is empty");
    ASSERT_MSG(event_type != NULL, "bus_client_check_typed: event_type is NULL");
    ASSERT_MSG(event_type[0] != '\0', "bus_client_check_typed: event_type is empty");
    ASSERT_MSG(target_handle != NULL, "bus_client_check_typed: target_handle is NULL");
    ASSERT_MSG(target_handle[0] != '\0', "bus_client_check_typed: target_handle is empty");
    ASSERT_MSG(strlen(target_handle) <= 253,
               "bus_client_check_typed: target_handle too long (%zu bytes, max 253). "
               "Would truncate @handle in 256-byte buffer.",
               strlen(target_handle));
    ASSERT_MSG(payload_out != NULL, "bus_client_check_typed: payload_out is NULL");
    ASSERT_MSG(payload_size > 0, "bus_client_check_typed: payload_size is 0");
    ASSERT_MSG(event_file_out != NULL, "bus_client_check_typed: event_file_out is NULL");
    ASSERT_MSG(event_file_size > 0, "bus_client_check_typed: event_file_size is 0");

    /* Initialise output */
    event_file_out[0] = '\0';

    /* Run nbs-bus check to get the event listing */
    const char *argv[] = {resolve_nbs_bus(), "check", bus_dir, NULL};
    char buf[CMD_BUF_SIZE];

    int rc = exec_capture(argv, buf, sizeof(buf));
    if (rc < 0) {
        fprintf(stderr, "bus_client_check_typed: exec_capture failed for '%s'\n",
                argv[0]);
        return -1;
    }
    if (rc != 0 || buf[0] == '\0')
        return 1; /* no events */

    /* Warn if output may have been truncated (events at end silently dropped) */
    if (strlen(buf) >= sizeof(buf) - 1) {
        fprintf(stderr, "bus_client_check_typed: output may be truncated "
                "(%zu bytes, buffer %zu) — some events may be missed\n",
                strlen(buf), sizeof(buf));
    }

    /* Build the @handle string for matching */
    char at_handle[256];
    int at_n = snprintf(at_handle, sizeof(at_handle), "@%s", target_handle);
    ASSERT_MSG(at_n >= 0 && (size_t)at_n < sizeof(at_handle),
               "bus_client_check_typed: at_handle truncated for handle '%s'",
               target_handle);

    /*
     * Parse output line by line. Each line is:
     *   [priority] filename (age)
     * Filter for lines containing event_type, extract filename,
     * read payload, check for @handle, return WITHOUT acking if match.
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
                if (fname_len >= sizeof(event_file)) {
                    fprintf(stderr, "bus_client_check_typed: event filename "
                            "truncated (%zu >= %zu), skipping\n",
                            fname_len, sizeof(event_file));
                    goto next_line;
                }
                memcpy(event_file, fname_start, fname_len);
                event_file[fname_len] = '\0';

                /* Read the event payload */
                char payload_buf[CMD_BUF_SIZE];
                if (bus_client_read(bus_dir, event_file,
                                    payload_buf, sizeof(payload_buf)) == 0) {
                    /* Check if @handle appears in the payload */
                    if (strstr(payload_buf, at_handle) != NULL) {
                        /* Copy payload to output */
                        snprintf(payload_out, payload_size, "%s", payload_buf);

                        /* Copy event filename to output for deferred ack.
                         * If the caller's buffer is too small, leave it empty
                         * — caller won't be able to ack but still gets payload. */
                        if (fname_len < event_file_size) {
                            memcpy(event_file_out, event_file, fname_len + 1);
                        } else {
                            fprintf(stderr, "bus_client_check_typed: event_file_out "
                                    "buffer too small (%zu < %zu), cannot store filename\n",
                                    event_file_size, fname_len + 1);
                            event_file_out[0] = '\0';
                        }

                        return 0; /* match found, NOT acked */
                    }
                } else {
                    fprintf(stderr, "bus_client_check_typed: failed to read "
                            "event '%s' from '%s'\n", event_file, bus_dir);
                }
            }
        }

next_line:
        /* Advance to next line */
        if (line_end != NULL)
            line_start = line_end + 1;
        else
            break;
    }

    return 1; /* no matching event */
}

int bus_client_ack_event(const char *bus_dir, const char *event_file)
{
    ASSERT_MSG(bus_dir != NULL, "bus_client_ack_event: bus_dir is NULL");
    ASSERT_MSG(bus_dir[0] != '\0', "bus_client_ack_event: bus_dir is empty");
    ASSERT_MSG(event_file != NULL, "bus_client_ack_event: event_file is NULL");
    ASSERT_MSG(event_file[0] != '\0', "bus_client_ack_event: event_file is empty");

    /* Validate filename: reject path traversal (../) and slashes */
    if (strstr(event_file, "..") != NULL || strchr(event_file, '/') != NULL) {
        fprintf(stderr, "bus_client_ack_event: invalid event filename '%s' "
                "(path traversal or slash detected)\n", event_file);
        return -1;
    }

    return bus_client_ack(bus_dir, event_file);
}
