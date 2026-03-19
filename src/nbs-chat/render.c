/*
 * render.c — Shared ANSI colour rendering for chat messages
 *
 * Extracted from terminal.c so that both nbs-chat-terminal and
 * nbs-chat export share a single rendering implementation.
 */

#include "render.h"
#include "chat_file.h"

#include <string.h>
#include <stdio.h>

/* --- ANSI colour palette --- */

static const char *COLOURS[] = {
    "38;5;39",   /* Blue */
    "38;5;208",  /* Orange */
    "38;5;41",   /* Green */
    "38;5;213",  /* Pink */
    "38;5;226",  /* Yellow */
    "38;5;87",   /* Cyan */
    "38;5;196",  /* Red */
    "38;5;147",  /* Lavender */
};
#define NUM_COLOURS 8
_Static_assert(sizeof(COLOURS) / sizeof(COLOURS[0]) == NUM_COLOURS,
               "NUM_COLOURS must match COLOURS array length");

/* Handle-to-colour mapping */
typedef struct {
    char handle[MAX_HANDLE_LEN];
    int colour_index;
} handle_colour_t;

static handle_colour_t handle_colours[MAX_PARTICIPANTS];
static int handle_colour_count = 0;
static int next_colour = 0;

void render_init(void) {
    handle_colour_count = 0;
    next_colour = 0;
}

const char *render_get_colour(const char *handle) {
    ASSERT_MSG(handle != NULL, "render_get_colour: handle is NULL");

    for (int i = 0; i < handle_colour_count; i++) {
        if (strcmp(handle_colours[i].handle, handle) == 0) {
            return COLOURS[handle_colours[i].colour_index];
        }
    }
    if (handle_colour_count < MAX_PARTICIPANTS) {
        int sn_ret = snprintf(handle_colours[handle_colour_count].handle,
                              MAX_HANDLE_LEN, "%s", handle);
        if (sn_ret < 0 || sn_ret >= MAX_HANDLE_LEN) {
            fprintf(stderr, "warning: handle truncated in colour table: "
                    "length %d exceeds %d\n", sn_ret, MAX_HANDLE_LEN - 1);
        }
        handle_colours[handle_colour_count].colour_index = next_colour;
        handle_colour_count++;

        ASSERT_MSG(handle_colour_count <= MAX_PARTICIPANTS,
                   "render_get_colour: handle_colour_count %d exceeds MAX_PARTICIPANTS %d",
                   handle_colour_count, MAX_PARTICIPANTS);

        int idx = next_colour;
        next_colour = (next_colour + 1) % NUM_COLOURS;
        return COLOURS[idx];
    }
    return COLOURS[0];
}

/* --- Timestamp formatting --- */

static void format_timestamp(time_t timestamp, char *buf, size_t buf_size) {
    buf[0] = '\0';
    if (timestamp > 0) {
        struct tm tm_buf;
        struct tm *tm = gmtime_r(&timestamp, &tm_buf);
        if (tm) {
            char ts[24];
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
            snprintf(buf, buf_size, "[%s] ", ts);
        }
    }
}

/* --- Message rendering --- */

void render_message(const char *handle, const char *content,
                    time_t timestamp, FILE *out) {
    ASSERT_MSG(handle != NULL, "render_message: handle is NULL");
    ASSERT_MSG(content != NULL, "render_message: content is NULL");
    ASSERT_MSG(out != NULL, "render_message: out is NULL");

    char ts_prefix[32];
    format_timestamp(timestamp, ts_prefix, sizeof(ts_prefix));

    const char *colour = render_get_colour(handle);
    fprintf(out, "  %s%s%s\033[%sm%s%s%s: %s\n",
            RENDER_DIM, ts_prefix, RENDER_RESET,
            colour, RENDER_BOLD, handle, RENDER_RESET, content);
}

void render_message_own(const char *handle, const char *content,
                        time_t timestamp, FILE *out) {
    ASSERT_MSG(handle != NULL, "render_message_own: handle is NULL");
    ASSERT_MSG(content != NULL, "render_message_own: content is NULL");
    ASSERT_MSG(out != NULL, "render_message_own: out is NULL");

    char ts_prefix[32];
    format_timestamp(timestamp, ts_prefix, sizeof(ts_prefix));

    const char *colour = render_get_colour(handle);
    fprintf(out, "  %s%s\033[%sm%s%s%s: %s%s\n",
            RENDER_DIM, ts_prefix, colour, handle,
            RENDER_RESET, RENDER_DIM, content, RENDER_RESET);
}
