/*
 * chatview.c — Shared scrollable chat view TUI library
 *
 * Extracted from nbs-chat-edit/editor.c. Provides the full-screen
 * message list, standard key navigation, regex search, and full message
 * viewing via nbs-md-viewer.
 *
 * Read-only by default. Editing keybindings (delete, truncate, undo,
 * write) are layered on top by consumers via chatview_set_key_handler().
 */

#define _GNU_SOURCE

#include "chatview.h"
#include "../nbs-chat/render.h"
#include "../nbs-chat/handle_styles.h"

#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* --- Terminal state (module-global) --- */

static struct termios g_orig_termios;
static int g_raw_mode = 0;

/* Shared terminal dimensions — updated by SIGWINCH handler */
static int g_term_rows = 24;
static int g_term_cols = 80;

static void chatview_disable_raw(void) {
    if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = 0;
    }
    /* Show cursor, leave alternate screen */
    write(STDOUT_FILENO, "\x1b[?25h\x1b[?1049l", 15);
}

static void chatview_enable_raw(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cflag |= (unsigned)(CS8);
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; /* 100ms timeout */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_mode = 1;
    /* Hide cursor, switch to alternate screen */
    write(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l", 15);
}

static void update_term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        g_term_rows = ws.ws_row;
        g_term_cols = ws.ws_col;
    }
}

static void handle_sigwinch(int sig) {
    (void)sig;
    update_term_size();
}

/* --- Key reading --- */

int chatview_read_key(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return CHATVIEW_KEY_NONE;

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                char seq2;
                if (read(STDIN_FILENO, &seq2, 1) != 1) return '\x1b';
                if (seq2 == '~') {
                    switch (seq[1]) {
                        case '5': return CHATVIEW_KEY_PAGE_UP;
                        case '6': return CHATVIEW_KEY_PAGE_DOWN;
                        case '1': return CHATVIEW_KEY_HOME;
                        case '4': return CHATVIEW_KEY_END;
                    }
                }
            }
            switch (seq[1]) {
                case 'A': return CHATVIEW_KEY_UP;
                case 'B': return CHATVIEW_KEY_DOWN;
                case 'C': return CHATVIEW_KEY_RIGHT;
                case 'D': return CHATVIEW_KEY_LEFT;
                case 'H': return CHATVIEW_KEY_HOME;
                case 'F': return CHATVIEW_KEY_END;
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return CHATVIEW_KEY_HOME;
                case 'F': return CHATVIEW_KEY_END;
            }
        }
        return '\x1b';
    }

    return (unsigned char)c;
}

/* --- Handle colouring (shared helper) --- */

static const char *handle_colour_str(const char *handle) {
    static char buf[NBS_STYLE_BUFSIZE];
    const nbs_style_t *style = nbs_handle_colour(handle);
    int n = nbs_style_start(style, buf, sizeof(buf));
    if (n <= 0) buf[0] = '\0';
    return buf;
}

/* --- Rendering --- */

static void chatview_render(chatview_t *cv) {
    char *buf = malloc((size_t)cv->term_rows * ((size_t)cv->term_cols + 128) + 8192);
    if (!buf) return;
    int off = 0;

    /* Move to top-left, clear screen */
    off += sprintf(buf + off, "\x1b[H\x1b[2J");

    int content_rows = cv->term_rows - 3; /* header + status bar + message bar */
    int msg_count = cv->state.message_count;

    /* Header */
    int new_count = chatview_new_count(cv);
    if (new_count > 0) {
        off += sprintf(buf + off, "%s %s (%d messages)%s %s(%d new)%s %s\r\n",
                       RENDER_REVERSE, cv->title, msg_count,
                       cv->dirty ? " [modified]" : "",
                       RENDER_YELLOW, new_count, RENDER_RESET RENDER_REVERSE,
                       RENDER_RESET);
    } else {
        off += sprintf(buf + off, "%s %s (%d messages)%s %s\r\n",
                       RENDER_REVERSE, cv->title, msg_count,
                       cv->dirty ? " [modified]" : "",
                       RENDER_RESET);
    }

    /* Messages */
    for (int row = 0; row < content_rows; row++) {
        int idx = cv->scroll_top + row;
        if (idx >= msg_count) {
            off += sprintf(buf + off, "%s~%s\r\n", RENDER_DIM, RENDER_RESET);
            continue;
        }

        const chat_message_t *msg = &cv->state.messages[idx];
        int is_cursor = (idx == cv->cursor);
        int is_deleted = (idx < cv->msg_flags_count &&
                          (cv->msg_flags[idx] & CHATVIEW_MSG_DELETED));
        const char *hcol;

        /* Bracket handles get their registered style colour */
        static char bracket_buf[NBS_STYLE_BUFSIZE];
        const nbs_style_t *bracket_style = handle_style_lookup(msg->handle);
        if (bracket_style) {
            nbs_style_start(bracket_style, bracket_buf, sizeof(bracket_buf));
            hcol = bracket_buf;
        } else {
            hcol = handle_colour_str(msg->handle);
        }

        /* Format timestamp */
        char ts[32] = "";
        if (msg->timestamp > 0) {
            struct tm tm;
            gmtime_r(&msg->timestamp, &tm);
            strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
        }

        /* Truncate content for display */
        char preview[512];
        size_t plen = msg->content_len;
        if (plen > (size_t)(cv->term_cols - 30))
            plen = (size_t)(cv->term_cols - 30);
        if (plen > sizeof(preview) - 1)
            plen = sizeof(preview) - 1;
        memcpy(preview, msg->content, plen);
        preview[plen] = '\0';
        /* Replace newlines with spaces for single-line display */
        for (char *p = preview; *p; p++)
            if (*p == '\n') *p = ' ';

        /* Render line */
        if (is_deleted) {
            off += sprintf(buf + off, "%s%s%s[%3d] %s %s: %s%s\r\n",
                           is_cursor ? RENDER_REVERSE : "",
                           RENDER_RED, RENDER_STRIKE,
                           idx + 1, ts, msg->handle, preview,
                           RENDER_RESET);
        } else {
            off += sprintf(buf + off, "%s%s[%3d]%s %s%s%s%s %s%s%s: %s%s\r\n",
                           is_cursor ? RENDER_REVERSE : "",
                           RENDER_DIM, idx + 1, RENDER_RESET,
                           is_cursor ? RENDER_REVERSE : "",
                           RENDER_DIM, ts, RENDER_RESET,
                           is_cursor ? RENDER_REVERSE : "",
                           hcol, msg->handle,
                           preview, RENDER_RESET);
        }
    }

    /* Status bar */
    char status_left[256];
    if (cv->dirty) {
        int del_count = 0;
        for (int i = 0; i < msg_count && i < cv->msg_flags_count; i++)
            if (cv->msg_flags[i] & CHATVIEW_MSG_DELETED) del_count++;
        if (del_count > 0)
            snprintf(status_left, sizeof(status_left),
                     " %d/%d | %s%d to delete%s",
                     cv->cursor + 1, msg_count, RENDER_RED, del_count,
                     RENDER_RESET RENDER_REVERSE);
        else
            snprintf(status_left, sizeof(status_left), " %d/%d",
                     cv->cursor + 1, msg_count);
    } else {
        snprintf(status_left, sizeof(status_left), " %d/%d",
                 cv->cursor + 1, msg_count);
    }

    char status_right[256];
    if (cv->search_valid)
        snprintf(status_right, sizeof(status_right), "/%s  ", cv->search);
    else
        status_right[0] = '\0';

    off += sprintf(buf + off, "\x1b[%d;1H%s%-*s%s%s",
                   cv->term_rows - 1, RENDER_REVERSE,
                   cv->term_cols, status_left,
                   status_right, RENDER_RESET);

    /* Message bar */
    const char *hint = cv->help_hint ? cv->help_hint :
        " /:search ?:search-back n:next N:prev Enter:view Esc:exit";
    off += sprintf(buf + off, "\x1b[%d;1H\x1b[2K%s", cv->term_rows,
                   cv->status[0] ? cv->status : hint);

    write(STDOUT_FILENO, buf, (size_t)off);
    free(buf);
}

/* --- Search --- */

int chatview_search_forward(const chatview_t *cv, int from) {
    if (!cv->search_valid) return -1;
    for (int i = from; i < cv->state.message_count; i++) {
        if (regexec(&cv->search_re, cv->state.messages[i].content,
                    0, NULL, 0) == 0)
            return i;
        if (regexec(&cv->search_re, cv->state.messages[i].handle,
                    0, NULL, 0) == 0)
            return i;
    }
    return -1;
}

int chatview_search_backward(const chatview_t *cv, int from) {
    if (!cv->search_valid) return -1;
    for (int i = from; i >= 0; i--) {
        if (regexec(&cv->search_re, cv->state.messages[i].content,
                    0, NULL, 0) == 0)
            return i;
        if (regexec(&cv->search_re, cv->state.messages[i].handle,
                    0, NULL, 0) == 0)
            return i;
    }
    return -1;
}

/*
 * prompt_search — Interactive search prompt.
 * direction: 1 = forward (/), -1 = backward (?)
 */
static void prompt_search(chatview_t *cv, int direction) {
    /* Show cursor, move to bottom */
    char prompt_char = (direction > 0) ? '/' : '?';
    char prompt_buf[64];
    int plen = sprintf(prompt_buf, "\x1b[?25h\x1b[%d;1H\x1b[2K%c",
                       cv->term_rows, prompt_char);
    write(STDOUT_FILENO, prompt_buf, (size_t)plen);

    /* Read search string */
    char input[256];
    int ilen = 0;
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) continue;
        if (c == '\r' || c == '\n') break;
        if (c == '\x1b' || c == 3) { /* ESC or Ctrl-C */
            ilen = 0;
            break;
        }
        if (c == 127 || c == 8) { /* backspace */
            if (ilen > 0) {
                ilen--;
                write(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }
        if (ilen < (int)sizeof(input) - 1 && c >= 32) {
            input[ilen++] = c;
            write(STDOUT_FILENO, &c, 1);
        }
    }
    input[ilen] = '\0';

    /* Hide cursor */
    write(STDOUT_FILENO, "\x1b[?25l", 6);

    if (ilen == 0) {
        chatview_set_status(cv, "Search cancelled.");
        return;
    }

    /* Compile regex */
    if (cv->search_valid) {
        regfree(&cv->search_re);
        cv->search_valid = 0;
    }
    if (regcomp(&cv->search_re, input,
                REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0) {
        chatview_set_status(cv, "Invalid regex: %s", input);
        return;
    }
    cv->search_valid = 1;
    snprintf(cv->search, sizeof(cv->search), "%s", input);

    /* Find match in the requested direction */
    int found;
    if (direction > 0) {
        found = chatview_search_forward(cv, cv->cursor);
        if (found < 0) found = chatview_search_forward(cv, 0);
    } else {
        found = chatview_search_backward(cv, cv->cursor);
        if (found < 0)
            found = chatview_search_backward(cv,
                        cv->state.message_count - 1);
    }

    if (found >= 0) {
        cv->cursor = found;
        chatview_set_status(cv, "%c%s", prompt_char, cv->search);
    } else {
        chatview_set_status(cv, "Pattern not found: %s", cv->search);
    }
}

/* --- Message view (via nbs-md-viewer) --- */

static void view_message(chatview_t *cv) {
    int msg_count = cv->state.message_count;
    if (cv->cursor < 0 || cv->cursor >= msg_count) return;

    const chat_message_t *msg = &cv->state.messages[cv->cursor];
    const char *hcol = handle_colour_str(msg->handle);

    char ts[64] = "";
    if (msg->timestamp > 0) {
        struct tm tm;
        gmtime_r(&msg->timestamp, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    }

    /* Build header for the viewer */
    char *header = malloc(512);
    if (!header) return;
    int ho = 0;
    ho += sprintf(header + ho, "## Message %d/%d\n\n", cv->cursor + 1, msg_count);
    ho += sprintf(header + ho, "**From:** %s  \n", msg->handle);
    ho += sprintf(header + ho, "**Time:** %s  \n", ts);

    if (cv->cursor < cv->msg_flags_count &&
        (cv->msg_flags[cv->cursor] & CHATVIEW_MSG_DELETED)) {
        ho += sprintf(header + ho, "**MARKED FOR DELETION**  \n");
    }
    ho += sprintf(header + ho, "\n---\n\n");

    /* Build full content: header + message content */
    size_t full_len = (size_t)ho + msg->content_len + 1;
    char *full = malloc(full_len);
    if (!full) { free(header); return; }
    memcpy(full, header, (size_t)ho);
    memcpy(full + ho, msg->content, msg->content_len);
    full[ho + (int)msg->content_len] = '\0';
    free(header);

    /* Try to pipe through nbs-md-viewer */
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        /* Temporarily exit raw mode for the child */
        chatview_disable_raw();

        pid_t pid = fork();
        if (pid == 0) {
            /* Child: nbs-md-viewer reads from pipe */
            close(pipefd[1]);
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
            execlp("nbs-md-viewer", "nbs-md-viewer", (char *)NULL);
            /* If nbs-md-viewer not found, fall through to simple display */
            _exit(127);
        } else if (pid > 0) {
            /* Parent: write content to pipe, then wait */
            close(pipefd[0]);
            write(pipefd[1], full, strlen(full));
            close(pipefd[1]);

            int wstatus;
            waitpid(pid, &wstatus, 0);

            if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 127) {
                /* nbs-md-viewer not found — fall back to simple display */
                goto simple_display;
            }
        } else {
            close(pipefd[0]);
            close(pipefd[1]);
            goto simple_display;
        }

        /* Re-enter raw mode */
        chatview_enable_raw();
        free(full);
        return;
    }

simple_display:
    /* Fallback: simple inline display (no nbs-md-viewer) */
    chatview_enable_raw();
    {
        char *vbuf = malloc(msg->content_len + 4096);
        if (!vbuf) { free(full); return; }
        int vo = 0;
        vo += sprintf(vbuf + vo, "\x1b[H\x1b[2J");
        vo += sprintf(vbuf + vo, "%s Message %d/%d %s\r\n\r\n",
                      RENDER_REVERSE, cv->cursor + 1, msg_count, RENDER_RESET);
        vo += sprintf(vbuf + vo, "  %sFrom:%s  %s%s%s\r\n",
                      RENDER_DIM, RENDER_RESET, hcol, msg->handle, RENDER_RESET);
        vo += sprintf(vbuf + vo, "  %sTime:%s  %s\r\n",
                      RENDER_DIM, RENDER_RESET, ts);
        if (cv->cursor < cv->msg_flags_count &&
            (cv->msg_flags[cv->cursor] & CHATVIEW_MSG_DELETED))
            vo += sprintf(vbuf + vo, "  %s%sMARKED FOR DELETION%s\r\n",
                          RENDER_RED, RENDER_BOLD, RENDER_RESET);
        vo += sprintf(vbuf + vo, "\r\n");

        /* Print content with line wrapping */
        const char *p = msg->content;
        int line_col = 0;
        while (*p) {
            if (*p == '\n') {
                vbuf[vo++] = '\r';
                vbuf[vo++] = '\n';
                line_col = 0;
            } else {
                vbuf[vo++] = *p;
                line_col++;
                if (line_col >= cv->term_cols - 1) {
                    vbuf[vo++] = '\r';
                    vbuf[vo++] = '\n';
                    line_col = 0;
                }
            }
            p++;
        }

        vo += sprintf(vbuf + vo, "\r\n\r\n%s-- Press any key to return --%s",
                      RENDER_DIM, RENDER_RESET);
        write(STDOUT_FILENO, vbuf, (size_t)vo);
        free(vbuf);
        while (chatview_read_key() == CHATVIEW_KEY_NONE) ;
    }
    free(full);
}

/* --- Help display --- */

static void show_help(chatview_t *cv) {
    char *hbuf = malloc(4096);
    if (!hbuf) return;
    int ho = 0;
    ho += sprintf(hbuf + ho, "\x1b[H\x1b[2J");
    ho += sprintf(hbuf + ho, "%s Chat View -- Help %s\r\n\r\n",
                  RENDER_REVERSE, RENDER_RESET);
    ho += sprintf(hbuf + ho, "  %sNavigation%s\r\n", RENDER_BOLD, RENDER_RESET);
    ho += sprintf(hbuf + ho, "    Up/Down            One message\r\n");
    ho += sprintf(hbuf + ho, "    Page Up/Down       One screen\r\n");
    ho += sprintf(hbuf + ho, "    Home               First message\r\n");
    ho += sprintf(hbuf + ho, "    End                Last message\r\n");
    ho += sprintf(hbuf + ho, "    /                  Search forward (regex)\r\n");
    ho += sprintf(hbuf + ho, "    ?                  Search backward (regex)\r\n");
    ho += sprintf(hbuf + ho, "    n                  Next match\r\n");
    ho += sprintf(hbuf + ho, "    N                  Previous match\r\n");
    ho += sprintf(hbuf + ho, "\r\n");
    ho += sprintf(hbuf + ho, "    Enter, v           View full message\r\n");
    ho += sprintf(hbuf + ho, "\r\n");
    ho += sprintf(hbuf + ho, "  %sExit%s\r\n", RENDER_BOLD, RENDER_RESET);
    ho += sprintf(hbuf + ho, "    Escape             Return\r\n");
    ho += sprintf(hbuf + ho, "\r\n");
    ho += sprintf(hbuf + ho, "  %sPress any key to return%s",
                  RENDER_DIM, RENDER_RESET);
    write(STDOUT_FILENO, hbuf, (size_t)ho);
    free(hbuf);
    (void)cv;
    while (chatview_read_key() == CHATVIEW_KEY_NONE) ;
}

/* --- Lifecycle --- */

chatview_t *chatview_init(const chat_state_t *state, const char *title) {
    chatview_t *cv = calloc(1, sizeof(chatview_t));
    if (!cv) return NULL;

    cv->state = *state; /* shallow copy — takes ownership of messages array */
    cv->title = strdup(title ? title : "chatview");
    if (!cv->title) { free(cv); return NULL; }

    cv->msg_flags_count = state->message_count;
    if (state->message_count > 0) {
        cv->msg_flags = calloc((size_t)state->message_count, sizeof(uint8_t));
        if (!cv->msg_flags) {
            free(cv->title);
            free(cv);
            return NULL;
        }
    }

    cv->initial_count = state->message_count;

    /* Start at the end */
    cv->cursor = state->message_count > 0 ? state->message_count - 1 : 0;
    cv->needs_redraw = 1;

    update_term_size();
    cv->term_rows = g_term_rows;
    cv->term_cols = g_term_cols;

    int content_rows = cv->term_rows - 3;
    cv->scroll_top = cv->cursor - content_rows + 1;
    if (cv->scroll_top < 0) cv->scroll_top = 0;

    return cv;
}

void chatview_free(chatview_t *cv) {
    if (!cv) return;
    if (cv->search_valid) regfree(&cv->search_re);
    free(cv->msg_flags);
    free(cv->title);
    chat_state_free(&cv->state);
    free(cv);
}

/* --- Event loop --- */

int chatview_run(chatview_t *cv) {
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigwinch;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, &old_sa);

    chatview_enable_raw();

    /* Flush any stale input bytes from the PTY.
     * When the caller (e.g. nbs-chat-terminal) sends /browse\r via nbs-ts,
     * bytes may arrive between the terminal's raw mode and chatview's raw
     * mode. tcsetattr(TCSAFLUSH) discards pending input, but there can be
     * a race where bytes arrive after the flush. Explicit tcflush here
     * ensures a clean slate before we start reading keys. */
    tcflush(STDIN_FILENO, TCIFLUSH);

    int content_rows;
    int poll_counter = 0;

    while (1) {
        update_term_size();
        if (cv->term_rows != g_term_rows || cv->term_cols != g_term_cols) {
            cv->term_rows = g_term_rows;
            cv->term_cols = g_term_cols;
            cv->needs_redraw = 1;
        }
        content_rows = cv->term_rows - 3;

        /* Keep cursor in scroll view */
        if (cv->cursor < cv->scroll_top)
            cv->scroll_top = cv->cursor;
        if (cv->cursor >= cv->scroll_top + content_rows)
            cv->scroll_top = cv->cursor - content_rows + 1;
        if (cv->scroll_top < 0) cv->scroll_top = 0;

        if (cv->needs_redraw) {
            chatview_render(cv);
            cv->needs_redraw = 0;
        }

        int key = chatview_read_key();

        /* Call poll callback periodically (every ~15 timeouts = ~1.5s) */
        if (key == CHATVIEW_KEY_NONE) {
            poll_counter++;
            if (poll_counter >= 15 && cv->poll_fn) {
                poll_counter = 0;
                cv->poll_fn(cv, cv->poll_data);
            }
            continue;
        }
        poll_counter = 0;
        cv->needs_redraw = 1;

        /* Let custom key handler have first crack */
        if (cv->key_handler) {
            int result = cv->key_handler(cv, key, cv->key_handler_data);
            if (result == CHATVIEW_KEY_HANDLED) continue;
            if (result == CHATVIEW_KEY_QUIT) break;
        }

        int msg_count = cv->state.message_count;

        switch (key) {
        case CHATVIEW_KEY_UP:
            if (cv->cursor > 0) cv->cursor--;
            cv->status[0] = '\0';
            break;

        case CHATVIEW_KEY_DOWN:
            if (cv->cursor < msg_count - 1) cv->cursor++;
            cv->status[0] = '\0';
            break;

        case CHATVIEW_KEY_PAGE_UP:
            cv->cursor -= content_rows;
            if (cv->cursor < 0) cv->cursor = 0;
            cv->status[0] = '\0';
            break;

        case CHATVIEW_KEY_PAGE_DOWN:
            cv->cursor += content_rows;
            if (cv->cursor >= msg_count) cv->cursor = msg_count - 1;
            cv->status[0] = '\0';
            break;

        case CHATVIEW_KEY_HOME:
            cv->cursor = 0;
            cv->scroll_top = 0;
            cv->status[0] = '\0';
            break;

        case CHATVIEW_KEY_END:
            cv->cursor = msg_count > 0 ? msg_count - 1 : 0;
            cv->status[0] = '\0';
            break;

        case '/': /* search forward */
            prompt_search(cv, 1);
            break;

        case '?': /* search backward */
            prompt_search(cv, -1);
            break;

        case 'n': /* next match */
            if (cv->search_valid) {
                int found = chatview_search_forward(cv, cv->cursor + 1);
                if (found < 0) found = chatview_search_forward(cv, 0);
                if (found >= 0) {
                    cv->cursor = found;
                    cv->status[0] = '\0';
                } else {
                    chatview_set_status(cv, "No more matches");
                }
            }
            break;

        case 'N': /* previous match */
            if (cv->search_valid) {
                int found = chatview_search_backward(cv, cv->cursor - 1);
                if (found < 0)
                    found = chatview_search_backward(cv, cv->state.message_count - 1);
                if (found >= 0) {
                    cv->cursor = found;
                    cv->status[0] = '\0';
                } else {
                    chatview_set_status(cv, "No more matches");
                }
            }
            break;

        case '\r': /* Enter - view full message */
        case 'v':
            view_message(cv);
            break;

        case 'h': /* help */
            show_help(cv);
            break;

        case '\x1b': /* Escape — exit browse */
            goto done;

        default:
            break;
        }
    }

done:
    chatview_disable_raw();
    sigaction(SIGWINCH, &old_sa, NULL);
    return cv->cursor;
}

/* --- State updates --- */

void chatview_update(chatview_t *cv, const chat_state_t *new_state) {
    /* Resize flags array if needed */
    if (new_state->message_count > cv->msg_flags_count) {
        uint8_t *new_flags = realloc(cv->msg_flags,
                                     (size_t)new_state->message_count * sizeof(uint8_t));
        if (new_flags) {
            /* Zero-init new entries */
            memset(new_flags + cv->msg_flags_count, 0,
                   (size_t)(new_state->message_count - cv->msg_flags_count) *
                   sizeof(uint8_t));
            cv->msg_flags = new_flags;
            cv->msg_flags_count = new_state->message_count;
        }
    }

    /* Free old state, adopt new */
    chat_state_free(&cv->state);
    cv->state = *new_state;

    /* Clamp cursor if message count dropped (e.g. archive) */
    if (cv->cursor >= new_state->message_count && new_state->message_count > 0)
        cv->cursor = new_state->message_count - 1;
    if (cv->cursor < 0) cv->cursor = 0;

    cv->needs_redraw = 1;
}

/* --- Search --- */

void chatview_search(chatview_t *cv, const char *pattern) {
    if (!pattern || !pattern[0]) return;

    if (cv->search_valid) {
        regfree(&cv->search_re);
        cv->search_valid = 0;
    }
    if (regcomp(&cv->search_re, pattern,
                REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0) {
        chatview_set_status(cv, "Invalid regex: %s", pattern);
        return;
    }
    cv->search_valid = 1;
    snprintf(cv->search, sizeof(cv->search), "%s", pattern);

    /* Jump to first match */
    int found = chatview_search_forward(cv, 0);
    if (found >= 0) {
        cv->cursor = found;
        chatview_set_status(cv, "/%s", cv->search);
    } else {
        chatview_set_status(cv, "Pattern not found: %s", cv->search);
    }
}

/* --- Extension points --- */

void chatview_set_key_handler(chatview_t *cv, chatview_key_handler_t handler,
                              void *userdata) {
    cv->key_handler = handler;
    cv->key_handler_data = userdata;
}

void chatview_set_poll(chatview_t *cv, chatview_poll_fn fn, void *userdata) {
    cv->poll_fn = fn;
    cv->poll_data = userdata;
}

/* --- Utility --- */

void chatview_set_status(chatview_t *cv, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cv->status, sizeof(cv->status), fmt, ap);
    va_end(ap);
    cv->needs_redraw = 1;
}

int chatview_reload(chatview_t *cv, const char *path) {
    chat_state_t new_state;
    if (chat_read(path, &new_state) != 0)
        return -1;
    chatview_update(cv, &new_state);
    return 0;
}
