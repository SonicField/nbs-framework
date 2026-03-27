/*
 * render.c — Shared ANSI colour rendering for chat messages
 *
 * Extracted from terminal.c so that both nbs-chat-terminal and
 * nbs-chat export share a single rendering implementation.
 *
 * Colour palette and handle-to-colour mapping are delegated to
 * nbs_term_attr (nbs-common) — the single source of truth for
 * terminal attributes and 256-colour codes.
 */

#include "render.h"
#include "chat_file.h"

#include <string.h>
#include <stdio.h>

void render_init(void) {
    nbs_handle_colours_init();
}

const char *render_get_colour(const char *handle) {
    ASSERT_MSG(handle != NULL, "render_get_colour: handle is NULL");

    /* Get the style, then generate its SGR parameter string.
     * render_get_colour returns just the parameter (e.g. "38;5;39")
     * for use inside \033[...m sequences assembled by callers. */
    const nbs_style_t *style = nbs_handle_colour(handle);
    static char param_buf[NBS_STYLE_BUFSIZE];

    /* Extract just the SGR params: strip \033[ prefix and m suffix */
    char full[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(style, full, sizeof(full));
    if (n > 3) {
        /* full is "\033[...m" — extract "..." */
        int param_len = n - 3; /* skip \033[ (2 bytes) and m (1 byte) */
        memcpy(param_buf, full + 2, (size_t)param_len);
        param_buf[param_len] = '\0';
    } else {
        /* Fallback */
        snprintf(param_buf, sizeof(param_buf), "38;5;39");
    }
    return param_buf;
}

/* --- Timestamp formatting --- */

static void format_timestamp(time_t timestamp, char *buf, size_t buf_size) {
    ASSERT_MSG(buf != NULL, "format_timestamp: buf is NULL");
    ASSERT_MSG(buf_size > 0, "format_timestamp: buf_size must be positive, got %zu", buf_size);
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

    /* Timestamp — dim on dark grey background */
    nbs_style_fstart(&NBS_STYLE_HUMAN_TIMESTAMP, out);
    fprintf(out, "  %s", ts_prefix);

    /* Handle — cream bold on dark grey background */
    nbs_style_freset(out);
    nbs_style_fstart(&NBS_STYLE_HUMAN_HANDLE, out);
    fprintf(out, "%s", handle);

    /* Content — light grey on dark grey background */
    nbs_style_freset(out);
    nbs_style_fstart(&NBS_STYLE_HUMAN_CONTENT, out);
    fprintf(out, ": %s", content);

    /* Fill rest of line with background, then reset */
    fputs("\033[K", out);
    nbs_style_freset(out);
    fputc('\n', out);
}

void render_message_medic(const char *handle, const char *content,
                          time_t timestamp, FILE *out) {
    ASSERT_MSG(handle != NULL, "render_message_medic: handle is NULL");
    ASSERT_MSG(content != NULL, "render_message_medic: content is NULL");
    ASSERT_MSG(out != NULL, "render_message_medic: out is NULL");

    char ts_prefix[32];
    format_timestamp(timestamp, ts_prefix, sizeof(ts_prefix));

    fprintf(out, "  %s%s%s", RENDER_DIM, ts_prefix, RENDER_RESET);
    nbs_style_fstart(&NBS_STYLE_MEDIC_WARNING, out);
    fprintf(out, "%s", handle);
    nbs_style_freset(out);
    fprintf(out, ": %s\n", content);
}
