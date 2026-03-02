/*
 * terminal.c — Interactive terminal client for nbs-chat
 *
 * Usage: nbs-chat-terminal <file> <handle>
 *
 * Controls:
 *   Type a message and press Enter to send.
 *   Arrow keys, Home, End, Delete for line editing.
 *   Backspace to delete backwards.
 *   Type /edit to compose in $EDITOR (for multi-line messages).
 *   Type /filter <handle> to show only one participant's messages.
 *   Type /unfilter to return to showing all messages.
 *   Type /help for all commands.
 *   Type /exit or Ctrl-C to exit.
 *
 * New messages from others appear automatically via background polling.
 *
 * Exit codes:
 *   0 - Clean exit
 *   1 - General error
 *   2 - Chat file not found
 *   4 - Invalid arguments
 */

/* execvpe requires _GNU_SOURCE on Linux */
#define _GNU_SOURCE

#include "chat_file.h"
#include "bus_bridge.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>

#include "watchdog.h"

/* --- Configuration --- */

#define POLL_INTERVAL_MS 1500  /* Background message poll interval */

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

#define BOLD  "\033[1m"
#define DIM   "\033[2m"
#define RESET "\033[0m"

/* Handle-to-colour mapping */
typedef struct {
    char handle[MAX_HANDLE_LEN];
    int colour_index;
} handle_colour_t;

static handle_colour_t handle_colours[MAX_PARTICIPANTS];
static int handle_colour_count = 0;
static int next_colour = 0;

static const char *get_colour(const char *handle) {
    /* Precondition */
    ASSERT_MSG(handle != NULL, "get_colour: handle is NULL");

    for (int i = 0; i < handle_colour_count; i++) {
        if (strcmp(handle_colours[i].handle, handle) == 0) {
            return COLOURS[handle_colours[i].colour_index];
        }
    }
    if (handle_colour_count < MAX_PARTICIPANTS) {
        int sn_ret = snprintf(handle_colours[handle_colour_count].handle,
                              MAX_HANDLE_LEN, "%s", handle);
        /* Detect truncation: snprintf returns the number of characters
         * that would have been written.  If >= MAX_HANDLE_LEN, the
         * handle was truncated and colour lookups may fail to match. */
        if (sn_ret < 0 || sn_ret >= MAX_HANDLE_LEN) {
            fprintf(stderr, "warning: handle truncated in colour table: "
                    "length %d exceeds %d\n", sn_ret, MAX_HANDLE_LEN - 1);
        }
        handle_colours[handle_colour_count].colour_index = next_colour;
        handle_colour_count++;

        /* Invariant: colour count within bounds */
        ASSERT_MSG(handle_colour_count <= MAX_PARTICIPANTS,
                   "get_colour: handle_colour_count %d exceeds MAX_PARTICIPANTS %d",
                   handle_colour_count, MAX_PARTICIPANTS);

        int idx = next_colour;
        next_colour = (next_colour + 1) % NUM_COLOURS;
        return COLOURS[idx];
    }
    return COLOURS[0];
}

/* --- Global state --- */

static const char *g_chat_file = NULL;
static const char *g_handle = NULL;
static int g_msg_count = 0;
static volatile sig_atomic_t g_quit = 0;
static char g_filter_handle[MAX_HANDLE_LEN] = {0};  /* empty = show all, non-empty = show only this handle */

/* Cursor row tracking for wrapped-line redraw */
static int g_cursor_row = 0;  /* Row of cursor relative to first row of input */

/* --- Watchdog daemon --- */

static watchdog_state_t g_watchdog;

/* Resolve project root by walking up from chat file to find .nbs/ directory */
static int resolve_project_root(const char *chat_path, char *out, size_t out_size) {
    /* Start from the directory containing the chat file */
    char dir[4096];
    int n = snprintf(dir, sizeof(dir), "%s", chat_path);
    if (n <= 0 || (size_t)n >= sizeof(dir)) return -1;

    /* Strip filename to get directory */
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else { dir[0] = '.'; dir[1] = '\0'; }

    /* Walk up looking for .nbs/ */
    for (int i = 0; i < 10; i++) {
        char probe[4096];
        snprintf(probe, sizeof(probe), "%s/.nbs", dir);
        struct stat st;
        if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Resolve to absolute path */
            char *real = realpath(dir, NULL);
            if (real) {
                int sn = snprintf(out, out_size, "%s", real);
                free(real);
                return (sn > 0 && (size_t)sn < out_size) ? 0 : -1;
            }
            return -1;
        }
        /* Go up one level */
        slash = strrchr(dir, '/');
        if (!slash) break;
        *slash = '\0';
    }
    return -1;
}

static void *watchdog_thread_fn(void *arg) {
    watchdog_state_t *ws = (watchdog_state_t *)arg;

    while (watchdog_is_enabled(ws)) {
        sleep(WATCHDOG_POLL_INTERVAL_S);
        if (!watchdog_is_enabled(ws)) break;

        /* Count alive agent sessions via popen */
        FILE *fp = popen(
            "tmux list-sessions -F '#{session_name}' 2>/dev/null | "
            "grep -c 'nbs-.*-live' 2>/dev/null || echo 0", "r");
        int count = 0;
        if (fp) {
            if (fscanf(fp, "%d", &count) != 1) count = 0;
            pclose(fp);
        }

        watchdog_decision_t d = watchdog_evaluate(ws, count, time(NULL));
        if (d == WATCHDOG_RESTART) {
            char cmd[8192];
            int sn = snprintf(cmd, sizeof(cmd),
                     "bash '%s/bin/nbs-chat-terminal-restart.sh' '%s' '%s' &",
                     ws->project_root, ws->project_root, ws->chat_path);
            if (sn > 0 && (size_t)sn < sizeof(cmd)) {
                (void)system(cmd);
            }
        }
        /* WATCHDOG_RATE_LIMITED: evaluate already set enabled=0.
         * Thread will exit on next loop iteration. */
    }
    return NULL;
}

/* --- Terminal width --- */

static int get_terminal_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    /* Fallback: terminal size detection failed — default to 80 columns */
    return 80;
}

/* --- Line editing state --- */

#define LINE_INIT_CAP 256

typedef struct {
    char *buf;       /* Line buffer (always null-terminated) */
    size_t len;      /* Number of characters in buffer */
    size_t cap;      /* Allocated capacity */
    size_t cursor;   /* Cursor position: 0..len */
} line_state_t;

static void line_state_init(line_state_t *ls) {
    ASSERT_MSG(ls != NULL, "line_state_init: ls is NULL");
    ls->cap = LINE_INIT_CAP;
    ls->buf = malloc(ls->cap);
    ASSERT_MSG(ls->buf != NULL, "line_state_init: malloc failed");
    ls->buf[0] = '\0';
    ls->len = 0;
    ls->cursor = 0;
}

static void line_state_reset(line_state_t *ls) {
    ASSERT_MSG(ls != NULL, "line_state_reset: ls is NULL");
    ASSERT_MSG(ls->buf != NULL, "line_state_reset: buf is NULL");
    ls->len = 0;
    ls->cursor = 0;
    ls->buf[0] = '\0';
}

static void line_state_free(line_state_t *ls) {
    if (ls) {
        free(ls->buf);
        ls->buf = NULL;
        ls->len = 0;
        ls->cap = 0;
        ls->cursor = 0;
    }
}

/* --- Escape sequence parser --- */

typedef enum {
    ESC_NONE,
    ESC_GOT_ESC,
    ESC_GOT_BRACKET
} esc_state_enum_t;

typedef struct {
    esc_state_enum_t state;
    int param;   /* Numeric parameter (-1 = none) */
} esc_parser_t;

/* --- Display functions --- */

static void format_message(const char *handle, const char *content,
                           const char *my_handle, time_t timestamp) {
    /* Preconditions */
    ASSERT_MSG(handle != NULL, "format_message: handle is NULL");
    ASSERT_MSG(content != NULL, "format_message: content is NULL");
    ASSERT_MSG(my_handle != NULL, "format_message: my_handle is NULL");

    /* Format timestamp prefix */
    char ts_prefix[32] = "";
    if (timestamp > 0) {
        struct tm tm_buf;
        struct tm *tm = gmtime_r(&timestamp, &tm_buf);
        if (tm) {
            char ts[24];
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
            snprintf(ts_prefix, sizeof(ts_prefix), "[%s] ", ts);
        }
    }

    const char *colour = get_colour(handle);
    if (strcmp(handle, my_handle) == 0) {
        /* Own messages slightly dimmer — timestamp dim, handle coloured+dim */
        printf("  %s%s\033[%sm%s%s%s: %s%s\n",
               DIM, ts_prefix, colour, handle, RESET, DIM, content, RESET);
    } else {
        /* Others — timestamp dim, handle bold+coloured */
        printf("  %s%s%s\033[%sm%s%s%s: %s\n",
               DIM, ts_prefix, RESET, colour, BOLD, handle, RESET, content);
    }
}

static void print_prompt(const char *handle) {
    printf("%s%s>%s ", BOLD, handle, RESET);
    fflush(stdout);
}

static void print_help(void) {
    printf("\n");
    printf("%sCommands:%s\n", BOLD, RESET);
    printf("  %s/edit%s       Open $EDITOR to compose a multi-line message\n", DIM, RESET);
    printf("  %s/search%s     Search message history (e.g. /search parser)\n", DIM, RESET);
    printf("  %s/filter%s     Show only one participant (e.g. /filter pythia)\n", DIM, RESET);
    printf("  %s/unfilter%s   Return to showing all messages\n", DIM, RESET);
    printf("  %s/shutdown%s   Send wrap-up message and disable auto-restart\n", DIM, RESET);
    printf("  %s/restart%s    Manually restart the agent team\n", DIM, RESET);
    printf("  %s/help%s       Show this help\n", DIM, RESET);
    printf("  %s/exit%s       Leave the chat\n", DIM, RESET);
    printf("\n");
    printf("%sInput:%s\n", BOLD, RESET);
    printf("  %sEnter%s        Send the message\n", DIM, RESET);
    printf("  %sArrow keys%s   Move cursor left/right within the line\n", DIM, RESET);
    printf("  %sHome/End%s     Jump to start/end of line\n", DIM, RESET);
    printf("  %sBackspace%s    Delete character before cursor\n", DIM, RESET);
    printf("  %sDelete%s       Delete character at cursor\n", DIM, RESET);
    printf("  %sCtrl-C%s       Exit\n", DIM, RESET);
    printf("\n");
    printf("New messages from others appear automatically.\n");
    printf("\n");
}

/* --- Line redraw --- */

/*
 * Redraw the current prompt + input line, positioning cursor correctly.
 * Handles lines that wrap past the terminal width by tracking which
 * visual row the cursor is on and using \033[J (clear to end of screen)
 * to clear all wrapped content.
 *
 * Uses:
 *   \033[<N>A - move cursor up N rows
 *   \r        - carriage return (column 0)
 *   \033[J    - clear from cursor to end of screen
 *   \033[<N>C - move cursor right N columns
 */
static void line_redraw(const line_state_t *ls, const char *handle) {
    ASSERT_MSG(ls != NULL, "line_redraw: ls is NULL");
    ASSERT_MSG(ls->buf != NULL, "line_redraw: buf is NULL");
    ASSERT_MSG(ls->cursor <= ls->len,
               "line_redraw: cursor %zu > len %zu", ls->cursor, ls->len);

    int tw = get_terminal_width();
    ASSERT_MSG(tw > 0,
               "line_redraw: terminal width must be positive, got %d"
               " — ioctl failure or invalid terminal", tw);

    /* Guard against handle length overflowing int arithmetic.
     * MAX_HANDLE_LEN is 64 so this should never fire in practice,
     * but defends against a corrupted or adversarial handle pointer. */
    size_t handle_len = strlen(handle);
    ASSERT_MSG(handle_len <= (size_t)(INT_MAX - 2),
               "line_redraw: handle length %zu would overflow int arithmetic",
               handle_len);
    int prompt_vlen = (int)handle_len + 2;  /* visible: "handle> " */

    /* Move cursor up to the first row of the input area */
    if (g_cursor_row > 0) {
        printf("\033[%dA", g_cursor_row);
    }
    /* Go to column 0 and clear from here to end of screen */
    printf("\r\033[J");

    /* Print prompt */
    print_prompt(handle);

    /* Print buffer content — loop on short writes.
     * Short writes to a terminal are rare but possible (e.g. signal
     * interruption).  We loop to ensure all content is written. */
    if (ls->len > 0) {
        size_t written = 0;
        while (written < ls->len) {
            ssize_t wr = write(STDOUT_FILENO, ls->buf + written,
                               ls->len - written);
            if (wr < 0) {
                if (errno == EINTR) continue;
                /* Write to stdout failed -- terminal may be disconnected.
                 * Log but do not abort; the main loop will detect POLLHUP. */
                fprintf(stderr, "warning: write to stdout failed: %s\n",
                        strerror(errno));
                break;
            }
            written += (size_t)wr;
        }
    }

    /* Calculate where the cursor needs to be vs where it is now.
     * After printing, the cursor is at the end of the content.
     * Both positions are measured in characters from the start.
     *
     * Terminal deferred-wrap: when output fills exactly to the last column,
     * the cursor stays on that column until the next character is printed.
     * This means a position at a multiple of tw is still on the previous
     * row, not the next one. We use (pos - 1) / tw for row calculation
     * when pos > 0 to account for this. */

    /* Overflow guards: ls->len and ls->cursor are size_t; adding to
     * prompt_vlen (int) could overflow int.  In practice MAX_HANDLE_LEN
     * is 64 and line buffers are bounded by available memory, but we
     * guard defensively.  Clamp to INT_MAX to avoid UB. */
    ASSERT_MSG(ls->len <= (size_t)(INT_MAX - prompt_vlen),
               "line_redraw: len %zu + prompt_vlen %d would overflow int",
               ls->len, prompt_vlen);
    ASSERT_MSG(ls->cursor <= (size_t)(INT_MAX - prompt_vlen),
               "line_redraw: cursor %zu + prompt_vlen %d would overflow int",
               ls->cursor, prompt_vlen);

    int end_abs = prompt_vlen + (int)ls->len;
    int target_abs = prompt_vlen + (int)ls->cursor;

    int end_row = (end_abs > 0) ? ((end_abs - 1) / tw) : 0;
    int target_row = (target_abs > 0) ? ((target_abs - 1) / tw) : 0;
    int target_col = target_abs % tw;

    /* Move up from end position to target row */
    int rows_up = end_row - target_row;
    if (rows_up > 0) {
        printf("\033[%dA", rows_up);
    }

    /* Position at target column */
    printf("\r");
    if (target_col > 0) {
        printf("\033[%dC", target_col);
    }

    fflush(stdout);

    /* Update tracking state */
    g_cursor_row = target_row;
}

/* --- Line editing operations --- */

static void line_ensure_cap(line_state_t *ls, size_t needed) {
    if (needed >= ls->cap) {
        /* Overflow guard: if needed is anywhere near SIZE_MAX / 2,
         * doubling will wrap around size_t.  Abort rather than
         * silently allocating a tiny (wrapped) buffer. */
        ASSERT_MSG(needed < SIZE_MAX / 2,
                   "line_ensure_cap: needed %zu is too large (overflow risk)",
                   needed);

        size_t new_cap = ls->cap;
        if (new_cap == 0) new_cap = LINE_INIT_CAP;
        while (new_cap <= needed) {
            ASSERT_MSG(new_cap <= SIZE_MAX / 2,
                       "line_ensure_cap: capacity %zu would overflow on doubling",
                       new_cap);
            new_cap *= 2;
        }
        char *newbuf = realloc(ls->buf, new_cap);
        ASSERT_MSG(newbuf != NULL, "line_ensure_cap: realloc failed for %zu", new_cap);
        ls->buf = newbuf;
        ls->cap = new_cap;
    }
}

static void line_insert_char(line_state_t *ls, char c) {
    ASSERT_MSG(ls != NULL, "line_insert_char: ls is NULL");
    ASSERT_MSG(ls->cursor <= ls->len,
               "line_insert_char: cursor %zu > len %zu", ls->cursor, ls->len);

    line_ensure_cap(ls, ls->len + 1);

    /* Shift characters right to make room at cursor */
    if (ls->cursor < ls->len) {
        memmove(ls->buf + ls->cursor + 1,
                ls->buf + ls->cursor,
                ls->len - ls->cursor);
    }
    ls->buf[ls->cursor] = c;
    ls->cursor++;
    ls->len++;
    ls->buf[ls->len] = '\0';

    /* Postcondition */
    ASSERT_MSG(ls->buf[ls->len] == '\0',
               "line_insert_char: not null-terminated at %zu", ls->len);
    ASSERT_MSG(ls->cursor <= ls->len,
               "line_insert_char: cursor %zu > len %zu after insert",
               ls->cursor, ls->len);
}

static void line_delete_back(line_state_t *ls) {
    ASSERT_MSG(ls != NULL, "line_delete_back: ls is NULL");
    if (ls->cursor == 0) return;

    /* Shift characters left over the deleted position */
    if (ls->cursor < ls->len) {
        memmove(ls->buf + ls->cursor - 1,
                ls->buf + ls->cursor,
                ls->len - ls->cursor);
    }
    ls->cursor--;
    ls->len--;
    ls->buf[ls->len] = '\0';

    /* Postcondition */
    ASSERT_MSG(ls->cursor <= ls->len,
               "line_delete_back: cursor %zu > len %zu", ls->cursor, ls->len);
}

static void line_delete_forward(line_state_t *ls) {
    ASSERT_MSG(ls != NULL, "line_delete_forward: ls is NULL");
    if (ls->cursor >= ls->len) return;

    /* Shift characters left over the deleted position */
    memmove(ls->buf + ls->cursor,
            ls->buf + ls->cursor + 1,
            ls->len - ls->cursor - 1);
    ls->len--;
    ls->buf[ls->len] = '\0';

    /* Postcondition */
    ASSERT_MSG(ls->cursor <= ls->len,
               "line_delete_forward: cursor %zu > len %zu", ls->cursor, ls->len);
}

static void line_move_left(line_state_t *ls) {
    ASSERT_MSG(ls != NULL, "line_move_left: ls is NULL");
    if (ls->cursor > 0) ls->cursor--;
}

static void line_move_right(line_state_t *ls) {
    ASSERT_MSG(ls != NULL, "line_move_right: ls is NULL");
    if (ls->cursor < ls->len) ls->cursor++;
}

static void line_move_home(line_state_t *ls) {
    ASSERT_MSG(ls != NULL, "line_move_home: ls is NULL");
    ls->cursor = 0;
}

static void line_move_end(line_state_t *ls) {
    ASSERT_MSG(ls != NULL, "line_move_end: ls is NULL");
    ls->cursor = ls->len;
}

/* --- Escape sequence handling --- */

/*
 * Process one byte of an escape sequence.
 * Returns 1 if the byte was consumed by the parser, 0 if not (normal char).
 */
static int handle_escape_input(line_state_t *ls, esc_parser_t *esc,
                               char c, const char *handle) {
    ASSERT_MSG(ls != NULL, "handle_escape_input: ls is NULL");
    ASSERT_MSG(esc != NULL, "handle_escape_input: esc is NULL");

    if (esc->state == ESC_NONE) {
        if (c == 0x1B) {
            esc->state = ESC_GOT_ESC;
            esc->param = -1;
            return 1;
        }
        return 0;
    }

    if (esc->state == ESC_GOT_ESC) {
        if (c == '[') {
            esc->state = ESC_GOT_BRACKET;
            esc->param = -1;
            return 1;
        }
        /* Not a CSI sequence (e.g. Alt+key) — discard */
        esc->state = ESC_NONE;
        return 1;
    }

    if (esc->state == ESC_GOT_BRACKET) {
        /* Accumulate numeric parameter */
        if (c >= '0' && c <= '9') {
            if (esc->param < 0) esc->param = 0;
            if (esc->param > 9999) {
                /* Reject unreasonably large escape parameters */
                esc->state = ESC_NONE;
                return 1;
            }
            esc->param = esc->param * 10 + (c - '0');
            return 1;
        }

        /* Dispatch on final character */
        switch (c) {
        case 'A': /* Up arrow — ignore */
            break;
        case 'B': /* Down arrow — ignore */
            break;
        case 'C': /* Right arrow */
            line_move_right(ls);
            line_redraw(ls, handle);
            break;
        case 'D': /* Left arrow */
            line_move_left(ls);
            line_redraw(ls, handle);
            break;
        case 'H': /* Home */
            line_move_home(ls);
            line_redraw(ls, handle);
            break;
        case 'F': /* End */
            line_move_end(ls);
            line_redraw(ls, handle);
            break;
        case '~': /* Dispatch on param */
            if (esc->param == 3) {
                /* Delete key */
                line_delete_forward(ls);
                line_redraw(ls, handle);
            } else if (esc->param == 1) {
                /* Home (alternate) */
                line_move_home(ls);
                line_redraw(ls, handle);
            } else if (esc->param == 4) {
                /* End (alternate) */
                line_move_end(ls);
                line_redraw(ls, handle);
            }
            /* Other param~ sequences ignored */
            break;
        default:
            /* Unknown sequence — discard */
            break;
        }

        esc->state = ESC_NONE;
        return 1;
    }

    /* Should not reach here */
    esc->state = ESC_NONE;
    return 1;
}

/* --- Case-insensitive substring search --- */

static const char *strcasestr_portable(const char *haystack, const char *needle) {
    ASSERT_MSG(haystack != NULL, "strcasestr_portable: haystack is NULL");
    ASSERT_MSG(needle != NULL, "strcasestr_portable: needle is NULL");

    if (needle[0] == '\0') return haystack;

    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) {
            return p;
        }
    }
    return NULL;
}

/* --- Non-destructive message display --- */

/*
 * Check for new messages and display them without disrupting user input.
 * Only clears and redraws when messages from others actually arrive.
 */
static void poll_and_display(line_state_t *ls, const char *handle) {
    ASSERT_MSG(g_chat_file != NULL, "poll_and_display: g_chat_file is NULL");
    ASSERT_MSG(g_msg_count >= 0,
               "poll_and_display: g_msg_count negative: %d", g_msg_count);

    chat_state_t state;
    if (chat_read(g_chat_file, &state) < 0) return;

    /* Auto-archive detection: if message_count dropped, the file was
     * rewritten with fewer messages (first 1000 moved to archive).
     * Reset our counter so we don't go permanently deaf. */
    if (state.message_count < g_msg_count) {
        /* Clear input line */
        if (g_cursor_row > 0) {
            printf("\033[%dA", g_cursor_row);
        }
        printf("\r\033[J");
        printf("  %s--- chat archived, %d messages remaining ---%s\n",
               DIM, state.message_count, RESET);
        g_msg_count = state.message_count;
        g_cursor_row = 0;
        line_redraw(ls, handle);
        chat_state_free(&state);
        return;
    }

    if (state.message_count <= g_msg_count) {
        chat_state_free(&state);
        return;
    }

    /* Check if any new messages should be displayed */
    int has_displayable = 0;
    for (int i = g_msg_count; i < state.message_count; i++) {
        if (strcmp(state.messages[i].handle, g_handle) == 0) continue;
        if (g_filter_handle[0] != '\0' &&
            strcmp(state.messages[i].handle, g_filter_handle) != 0) continue;
        has_displayable = 1;
        break;
    }

    if (!has_displayable) {
        g_msg_count = state.message_count;
        chat_state_free(&state);
        return;
    }

    /* Clear the current input line (may span multiple visual rows) */
    if (g_cursor_row > 0) {
        printf("\033[%dA", g_cursor_row);
    }
    printf("\r\033[J");

    /* Display new messages (filtered if g_filter_handle is set) */
    for (int i = g_msg_count; i < state.message_count; i++) {
        if (strcmp(state.messages[i].handle, g_handle) == 0) continue;
        if (g_filter_handle[0] != '\0' &&
            strcmp(state.messages[i].handle, g_filter_handle) != 0) continue;
        format_message(state.messages[i].handle,
                      state.messages[i].content, g_handle,
                      state.messages[i].timestamp);
    }

    g_msg_count = state.message_count;
    chat_state_free(&state);

    /* Restore prompt and user input — cursor starts from fresh line */
    g_cursor_row = 0;
    line_redraw(ls, handle);
}

/* --- Send helper --- */

/*
 * do_send — Shared send logic: chat_send + msg_count + bus events.
 *
 * Returns 0 on success, -1 on send failure.
 * This is the single source of truth for post-send side effects.
 */
static int do_send(const char *msg) {
    ASSERT_MSG(msg != NULL, "do_send: msg is NULL");
    ASSERT_MSG(g_chat_file != NULL, "do_send: g_chat_file is NULL");
    ASSERT_MSG(g_handle != NULL, "do_send: g_handle is NULL");

    if (chat_send(g_chat_file, g_handle, msg) != 0) {
        return -1;
    }

    /* Guard against signed overflow on g_msg_count.  In practice
     * MAX_MESSAGES is 10000 so this should never fire, but a corrupt
     * or adversarial chat file could push the count toward INT_MAX. */
    ASSERT_MSG(g_msg_count < INT_MAX,
               "do_send: g_msg_count overflow: %d", g_msg_count);
    g_msg_count++;

    /* Publish bus events: standard chat-message + human-input priority signal */
    bus_bridge_after_send(g_chat_file, g_handle, msg);
    bus_bridge_human_input(g_chat_file, g_handle, msg);

    return 0;
}

static void send_and_display(line_state_t *ls) {
    ASSERT_MSG(ls != NULL, "send_and_display: ls is NULL");
    ASSERT_MSG(ls->len > 0, "send_and_display: called with empty buffer");

    if (do_send(ls->buf) != 0) {
        printf("  %s(send failed)%s\n", DIM, RESET);
    }
}

/* --- Editor mode --- */

/*
 * Validate EDITOR value against an allowlist of known editors, then
 * fall back to rejecting shell metacharacters for unlisted-but-safe
 * editors (e.g. micro, helix).  This prevents command injection via
 * EDITOR="vi; rm -rf /" being passed to execlp.
 */
static int editor_is_valid(const char *editor) {
    if (!editor || editor[0] == '\0') return 0;

    /* Extract basename for allowlist comparison */
    const char *base = strrchr(editor, '/');
    base = base ? base + 1 : editor;

    const char *allowed[] = {
        "vi", "vim", "nvim", "nano", "emacs", "ed", NULL
    };
    for (int i = 0; allowed[i] != NULL; i++) {
        if (strcmp(base, allowed[i]) == 0) return 1;
    }

    /* Not in allowlist -- reject if any shell metacharacter present */
    const char *bad = ";|&$`\\\"'(){}[]<>!~#*? \t\n\r";
    for (const char *p = editor; *p; p++) {
        if (strchr(bad, *p) != NULL) return 0;
    }

    /* Unlisted but no metacharacters -- accept */
    return 1;
}

static char *open_editor(void) {
    const char *editor = getenv("EDITOR");
    if (!editor || !editor_is_valid(editor)) editor = "vim";

    /* Create temp file with restricted permissions.
     * mkstemp creates the file, but POSIX does not guarantee 0600 —
     * the result depends on the process umask.  Explicitly set 0600
     * to prevent other users from reading the draft message. */
    char tmppath[] = "/tmp/nbs-chat-edit.XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return NULL;
    if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        /* Cannot restrict permissions — refuse to use the file */
        close(fd);
        unlink(tmppath);
        return NULL;
    }
    if (close(fd) != 0) {
        unlink(tmppath);
        return NULL;
    }

    /* Fork and exec editor */
    pid_t pid = fork();
    if (pid < 0) {
        unlink(tmppath);
        return NULL;
    }

    if (pid == 0) {
        /* Child: run editor with /dev/tty and sanitised environment.
         * Only PATH, HOME, TERM, and LANG are passed to the editor
         * to prevent leaking sensitive environment variables. */
        int tty = open("/dev/tty", O_RDWR);
        if (tty < 0) {
            fprintf(stderr, "error: cannot open /dev/tty for editor: %s\n",
                    strerror(errno));
            _exit(1);
        }
        if (dup2(tty, STDIN_FILENO) < 0) {
            fprintf(stderr, "error: dup2 failed for editor stdin: %s\n",
                    strerror(errno));
            close(tty);
            _exit(1);
        }
        close(tty);

        /* Build sanitised environment — only safe variables */
        const char *safe_vars[] = {"PATH", "HOME", "TERM", "LANG", NULL};
        char *clean_env[5];  /* 4 vars + NULL terminator */
        int env_count = 0;
        for (int i = 0; safe_vars[i] != NULL; i++) {
            const char *val = getenv(safe_vars[i]);
            if (val) {
                /* Format: NAME=VALUE */
                size_t name_len = strlen(safe_vars[i]);
                size_t val_len = strlen(val);
                char *entry = malloc(name_len + 1 + val_len + 1);
                if (entry) {
                    snprintf(entry, name_len + 1 + val_len + 1,
                             "%s=%s", safe_vars[i], val);
                    clean_env[env_count++] = entry;
                } else {
                    fprintf(stderr, "error: malloc failed for env var %s, "
                            "editor cannot function without environment\n",
                            safe_vars[i]);
                    _exit(1);
                }
            }
        }
        clean_env[env_count] = NULL;

        /* Use execvpe to search PATH with sanitised environment.
         * execle does NOT search PATH, so "vim" without a full path
         * would fail silently (exit 127), causing "(empty — not sent)". */
        char *argv[] = {(char *)editor, tmppath, NULL};
        execvpe(editor, argv, clean_env);
        for (int i = 0; i < env_count; i++) free(clean_env[i]);
        _exit(127);
    }

    /* Parent: wait for editor */
    int wstatus;
    pid_t wait_ret = waitpid(pid, &wstatus, 0);
    if (wait_ret < 0) {
        unlink(tmppath);
        return NULL;
    }

    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        unlink(tmppath);
        return NULL;
    }

    /* Read result -- use binary mode ("rb") so fseek/ftell give byte
     * counts rather than opaque text-mode positions.  On platforms where
     * text mode translates \r\n, the ftell offset is not necessarily a
     * byte count, causing incorrect malloc size and fread length. */
    FILE *f = fopen(tmppath, "rb");
    if (!f) {
        unlink(tmppath);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        unlink(tmppath);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        /* ftell failed — cannot determine file size */
        fclose(f);
        unlink(tmppath);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        unlink(tmppath);
        return NULL;
    }

    if (len == 0) {
        fclose(f);
        unlink(tmppath);
        return NULL;
    }

    /* Guard against len + 1 overflow on ILP32 where long == int (32-bit).
     * Also cap at MAX_MESSAGE_LEN for sanity — editor output beyond
     * 1 MB is almost certainly an error. */
    if (len > MAX_MESSAGE_LEN || (unsigned long)len >= SIZE_MAX) {
        fclose(f);
        unlink(tmppath);
        fprintf(stderr, "warning: editor file too large (%ld bytes, max %d)\n",
                len, MAX_MESSAGE_LEN);
        return NULL;
    }

    char *content = malloc((size_t)len + 1);
    if (!content) {
        fclose(f);
        unlink(tmppath);
        return NULL;
    }

    size_t nread = fread(content, 1, len, f);
    if (nread == 0 && ferror(f)) {
        /* Read error — no data recovered */
        free(content);
        fclose(f);
        unlink(tmppath);
        return NULL;
    }
    content[nread] = '\0';
    fclose(f);
    unlink(tmppath);

    /* Trim trailing newlines */
    while (nread > 0 && (content[nread - 1] == '\n' || content[nread - 1] == '\r')) {
        content[--nread] = '\0';
    }

    if (nread == 0) {
        free(content);
        return NULL;
    }

    return content;
}

/* --- Signal handling --- */

static void handle_signal(int sig) {
    (void)sig;
    g_quit = 1;
}

/* --- Main --- */

static void print_usage(void) {
    printf("nbs-chat-terminal: Interactive terminal client for nbs-chat\n\n");
    printf("Usage:\n");
    printf("  nbs-chat-terminal <file> <handle>\n\n");
    printf("  <file>    Path to chat file (must exist)\n");
    printf("  <handle>  Your display name in the chat\n\n");
    printf("Controls:\n");
    printf("  Type a message and press Enter to send.\n");
    printf("  Use arrow keys, Home, End, Delete for line editing.\n");
    printf("  Type /edit to compose multi-line messages in $EDITOR.\n");
    printf("  Type /help for all commands.\n");
    printf("  Type /exit or Ctrl-C to exit.\n\n");
    printf("New messages from others appear automatically.\n");
}

int main(int argc, char **argv) {
    if (argc < 3) {
        print_usage();
        return 4;
    }

    g_chat_file = argv[1];
    g_handle = argv[2];

    /* Preconditions: args validated from argv */
    ASSERT_MSG(g_chat_file != NULL, "main: chat_file path is NULL");
    ASSERT_MSG(g_handle != NULL, "main: handle is NULL");

    /* Validate handle is ASCII-only.  The cursor positioning arithmetic
     * in line_redraw uses strlen (byte count) as display column count.
     * Multi-byte UTF-8 characters would cause byte count != display width,
     * corrupting cursor positioning.  Reject non-ASCII handles at startup
     * rather than silently producing garbled output. */
    for (const char *p = g_handle; *p; p++) {
        if ((unsigned char)*p > 127) {
            fprintf(stderr, "Error: Handle must be ASCII-only (got non-ASCII "
                    "byte 0x%02x at position %td).\n"
                    "Non-ASCII handles break cursor positioning.\n",
                    (unsigned char)*p, p - g_handle);
            return 4;
        }
    }

    /* Check file exists */
    struct stat st;
    if (stat(g_chat_file, &st) != 0) {
        fprintf(stderr, "Error: Chat file not found: %s\n", g_chat_file);
        fprintf(stderr, "Create it first: nbs-chat create %s\n", g_chat_file);
        return 2;
    }

    /* Set up signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        fprintf(stderr, "warning: sigaction(SIGINT) failed: %s\n",
                strerror(errno));
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        fprintf(stderr, "warning: sigaction(SIGTERM) failed: %s\n",
                strerror(errno));
    }

    /* Put terminal in raw-ish mode (disable echo and canonical mode,
     * but keep signal generation for Ctrl-C) */
    struct termios orig_termios, raw;
    int have_termios = 0;
    if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
        have_termios = 1;
        raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_lflag |= ISIG;  /* Explicitly ensure Ctrl-C generates SIGINT */
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
            fprintf(stderr, "warning: tcsetattr(raw) failed: %s\n",
                    strerror(errno));
        }
    }

    /* Show existing messages */
    chat_state_t init_state;
    if (chat_read(g_chat_file, &init_state) == 0) {
        for (int i = 0; i < init_state.message_count; i++) {
            format_message(init_state.messages[i].handle,
                          init_state.messages[i].content, g_handle,
                          init_state.messages[i].timestamp);
        }
        g_msg_count = init_state.message_count;
        if (init_state.message_count > 0) printf("\n");
        chat_state_free(&init_state);
    }

    /* Initialise line editing state */
    line_state_t edit;
    line_state_init(&edit);
    esc_parser_t esc = { .state = ESC_NONE, .param = -1 };

    /* Print initial prompt */
    print_prompt(g_handle);

    /* --- Start watchdog daemon thread --- */
    char wd_project_root[4096];
    if (resolve_project_root(g_chat_file, wd_project_root,
                              sizeof(wd_project_root)) == 0) {
        watchdog_init(&g_watchdog, g_chat_file, wd_project_root);
        pthread_t watchdog_tid;
        if (pthread_create(&watchdog_tid, NULL, watchdog_thread_fn,
                           &g_watchdog) == 0) {
            pthread_detach(watchdog_tid);
        } else {
            fprintf(stderr, "warning: failed to start watchdog thread\n");
        }
    } else {
        fprintf(stderr, "warning: could not resolve project root from %s "
                        "— watchdog disabled\n", g_chat_file);
    }

    /* --- Event loop --- */
    while (!g_quit) {
        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
        int ready = poll(&pfd, 1, POLL_INTERVAL_MS);

        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Timeout: poll for new messages */
        if (ready == 0) {
            poll_and_display(&edit, g_handle);
            continue;
        }

        /* Read input if available — prioritise POLLIN over POLLHUP
         * because on pipes both can be set simultaneously when data
         * remains in the buffer after the write end closes. */
        if (!(pfd.revents & POLLIN)) {
            /* No data to read — check for hangup/error */
            if (pfd.revents & (POLLHUP | POLLERR)) {
                if (edit.len > 0) {
                    printf("\n");
                    send_and_display(&edit);
                }
                break;
            }
            continue;
        }

        /* Input available */
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            if (n == 0) {
                /* EOF: send pending input if any */
                if (edit.len > 0) {
                    printf("\n");
                    send_and_display(&edit);
                }
                break;
            }
            if (errno != EINTR && errno != EAGAIN) break;
            continue;
        }

        /* Escape sequence handling */
        if (handle_escape_input(&edit, &esc, c, g_handle)) {
            continue;
        }

        /* Enter: submit immediately */
        if (c == '\n' || c == '\r') {
            printf("\n");
            g_cursor_row = 0;  /* Newline resets cursor to fresh line */

            if (edit.len == 0) {
                /* Empty line: just reprint prompt, also poll */
                poll_and_display(&edit, g_handle);
                print_prompt(g_handle);
                continue;
            }

            /* Check for commands */
            if (strcmp(edit.buf, "/exit") == 0) {
                line_state_free(&edit);
                if (have_termios) {
                    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) != 0) {
                        fprintf(stderr, "warning: tcsetattr(restore) failed: %s\n",
                                strerror(errno));
                    }
                }
                printf("%sLeft chat.%s\n", DIM, RESET);
                return 0;
            }

            if (strcmp(edit.buf, "/help") == 0) {
                line_state_reset(&edit);
                print_help();
                print_prompt(g_handle);
                continue;
            }

            if (strcmp(edit.buf, "/edit") == 0) {
                line_state_reset(&edit);
                /* Restore terminal for editor */
                if (have_termios) {
                    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) != 0) {
                        fprintf(stderr, "warning: tcsetattr(restore for editor) failed: %s\n",
                                strerror(errno));
                    }
                }
                char *msg = open_editor();
                /* Back to raw mode. Reset terminal state after editor:
                 * - Leave alternate screen buffer if vim entered it
                 * - Reset cursor visibility
                 * - Clear from cursor to end of screen
                 * - Move to column 0 for clean prompt */
                printf("\033[?1049l");  /* leave alternate screen */
                printf("\033[?25h");    /* show cursor */
                printf("\033[0m");      /* reset attributes */
                printf("\r\n");         /* fresh line */
                fflush(stdout);
                if (have_termios) {
                    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
                        fprintf(stderr, "warning: tcsetattr(raw after editor) failed: %s\n",
                                strerror(errno));
                    }
                }
                if (msg) {
                    if (do_send(msg) == 0) {
                        format_message(g_handle, msg, g_handle, time(NULL));
                    } else {
                        printf("  %s(send failed)%s\n", DIM, RESET);
                    }
                    free(msg);
                } else {
                    printf("  %s(empty — not sent)%s\n", DIM, RESET);
                }
                /* Check for messages that arrived during editing */
                poll_and_display(&edit, g_handle);
                print_prompt(g_handle);
                continue;
            }

            if (strncmp(edit.buf, "/search ", 8) == 0) {
                const char *pattern = edit.buf + 8;
                /* Skip leading whitespace */
                while (*pattern == ' ') pattern++;

                if (*pattern == '\0') {
                    printf("  %sUsage: /search <pattern>%s\n", DIM, RESET);
                } else {
                    chat_state_t search_state;
                    if (chat_read(g_chat_file, &search_state) == 0) {
                        int match_count = 0;
                        for (int si = 0; si < search_state.message_count; si++) {
                            if (strcasestr_portable(search_state.messages[si].content,
                                                    pattern) != NULL) {
                                printf("  %s[%d]%s ", DIM, si, RESET);
                                format_message(search_state.messages[si].handle,
                                              search_state.messages[si].content,
                                              g_handle,
                                              search_state.messages[si].timestamp);
                                match_count++;
                            }
                        }
                        if (match_count == 0) {
                            printf("  %sNo matches found.%s\n", DIM, RESET);
                        } else {
                            printf("  %s%d match(es)%s\n", DIM, match_count, RESET);
                        }
                        chat_state_free(&search_state);
                    } else {
                        printf("  %s(search failed — could not read chat)%s\n",
                               DIM, RESET);
                    }
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            if (strcmp(edit.buf, "/search") == 0) {
                printf("  %sUsage: /search <pattern>%s\n", DIM, RESET);
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* /filter <handle> — show only messages from one participant */
            if (strncmp(edit.buf, "/filter ", 8) == 0) {
                const char *target = edit.buf + 8;
                while (*target == ' ') target++;
                if (*target == '\0') {
                    printf("  %sUsage: /filter <handle>%s\n", DIM, RESET);
                } else {
                    int sn = snprintf(g_filter_handle, sizeof(g_filter_handle), "%s", target);
                    if (sn < 0 || (size_t)sn >= sizeof(g_filter_handle)) {
                        printf("  %swarning: filter handle truncated to %d chars "
                               "(max handle length is %d)%s\n",
                               DIM, (int)(sizeof(g_filter_handle) - 1),
                               MAX_HANDLE_LEN - 1, RESET);
                    }
                    printf("  %sFiltering: showing only messages from %s%s\n",
                           DIM, g_filter_handle, RESET);
                    /* Redisplay last 50 matching messages (most recent first, then reverse) */
                    chat_state_t fstate;
                    if (chat_read(g_chat_file, &fstate) == 0) {
                        /* Collect indices of matching messages */
                        int matches[50];
                        int match_count = 0;
                        for (int i = fstate.message_count - 1; i >= 0 && match_count < 50; i--) {
                            if (strcmp(fstate.messages[i].handle, g_filter_handle) == 0) {
                                matches[match_count++] = i;
                            }
                        }
                        /* Display in chronological order (reverse the collected indices) */
                        for (int j = match_count - 1; j >= 0; j--) {
                            int i = matches[j];
                            format_message(fstate.messages[i].handle,
                                          fstate.messages[i].content, g_handle,
                                          fstate.messages[i].timestamp);
                        }
                        if (match_count == 0) {
                            printf("  %sNo messages from '%s'%s\n",
                                   DIM, g_filter_handle, RESET);
                        }
                        chat_state_free(&fstate);
                    }
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            if (strcmp(edit.buf, "/filter") == 0) {
                if (g_filter_handle[0] != '\0') {
                    printf("  %sCurrently filtering: %s%s\n",
                           DIM, g_filter_handle, RESET);
                } else {
                    printf("  %sNo filter active. Usage: /filter <handle>%s\n",
                           DIM, RESET);
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* /unfilter — return to showing all messages */
            if (strcmp(edit.buf, "/unfilter") == 0) {
                if (g_filter_handle[0] != '\0') {
                    g_filter_handle[0] = '\0';
                    printf("  %sFilter cleared — showing last 20 messages%s\n",
                           DIM, RESET);
                    /* Redisplay recent messages so the screen makes sense */
                    chat_state_t ustate;
                    if (chat_read(g_chat_file, &ustate) == 0) {
                        int start = ustate.message_count - 20;
                        if (start < 0) start = 0;
                        for (int i = start; i < ustate.message_count; i++) {
                            format_message(ustate.messages[i].handle,
                                          ustate.messages[i].content, g_handle,
                                          ustate.messages[i].timestamp);
                        }
                        chat_state_free(&ustate);
                    }
                } else {
                    printf("  %sNo filter active%s\n", DIM, RESET);
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* /shutdown — send wrap-up message, disable watchdog */
            if (strcmp(edit.buf, "/shutdown") == 0) {
                do_send("@team Good work — time to wrap up. "
                        "Please commit any uncommitted changes, "
                        "post a final session summary, and shut down cleanly.");
                watchdog_disable(&g_watchdog);
                printf("  %sWatchdog disabled. Team will not be auto-restarted.%s\n",
                       DIM, RESET);
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* /restart — manual team restart (bypasses rate limit) */
            if (strcmp(edit.buf, "/restart") == 0) {
                printf("  %sTriggering manual restart...%s\n", DIM, RESET);
                char rcmd[8192];
                int rsn = snprintf(rcmd, sizeof(rcmd),
                         "bash '%s/bin/nbs-chat-terminal-restart.sh' '%s' '%s' &",
                         g_watchdog.project_root, g_watchdog.project_root,
                         g_watchdog.chat_path);
                if (rsn > 0 && (size_t)rsn < sizeof(rcmd)) {
                    (void)system(rcmd);
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* Regular message: send immediately */
            send_and_display(&edit);
            line_state_reset(&edit);
            /* Check for messages after sending */
            poll_and_display(&edit, g_handle);
            print_prompt(g_handle);
            continue;
        }

        /* Ctrl-D */
        if (c == 4) {
            if (edit.len == 0) {
                break;
            }
            /* Send pending and exit */
            printf("\n");
            send_and_display(&edit);
            break;
        }

        /* Ctrl-C */
        if (c == 3) {
            g_quit = 1;
            if (edit.len > 0) {
                printf("\n");
                send_and_display(&edit);
            }
            break;
        }

        /* Backspace / DEL */
        if (c == 127 || c == 8) {
            if (edit.cursor > 0) {
                line_delete_back(&edit);
                line_redraw(&edit, g_handle);
            }
            continue;
        }

        /* Ignore other control chars except tab */
        if (c < 32 && c != '\t') continue;

        /* Printable character: insert at cursor */
        line_insert_char(&edit, c);
        line_redraw(&edit, g_handle);
    }

    /* Cleanup */
    line_state_free(&edit);
    if (have_termios) {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) != 0) {
            fprintf(stderr, "warning: tcsetattr(final restore) failed: %s\n",
                    strerror(errno));
        }
    }
    printf("\n%sLeft chat.%s\n", DIM, RESET);

    return 0;
}
