/*
 * editor.c — Interactive terminal editor for nbs-chat files.
 *
 * Usage: nbs-chat-edit <file>
 *
 * Navigation:
 *   Up/Down, j/k     One message at a time
 *   Page Up/Down      One screen at a time
 *   Home/g            Go to first message
 *   End/G             Go to last message
 *   /                 Search forward (regex)
 *   n                 Next search match
 *   N                 Previous search match
 *
 * Editing:
 *   d                 Mark/unmark message for deletion
 *   t                 Mark this message and all after for deletion (truncate)
 *   u                 Undo last action
 *   Ctrl-R            Redo
 *
 * File:
 *   w                 Write changes (atomic rewrite)
 *   q                 Quit (warns if unsaved)
 *   Q                 Quit without saving
 *
 * Exit codes:
 *   0 - Clean exit
 *   1 - Error
 *   4 - Invalid arguments
 */

#define _GNU_SOURCE

#include "../nbs-chat/chat_file.h"
#include "../nbs-chat/render.h"

#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* --- Terminal --- */

static struct termios g_orig_termios;
static int g_raw_mode = 0;
static int g_term_rows = 24;
static int g_term_cols = 80;

static void disable_raw_mode(void) {
    if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = 0;
    }
    /* Show cursor, clear alternate screen */
    write(STDOUT_FILENO, "\x1b[?25h\x1b[?1049l", 15);
}

static void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
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

/* --- Undo/Redo --- */

#define MAX_UNDO 256

typedef struct {
    int index;      /* message index */
    int was_deleted; /* previous state */
} undo_entry_t;

static undo_entry_t g_undo_stack[MAX_UNDO];
static int g_undo_top = 0;
static undo_entry_t g_redo_stack[MAX_UNDO];
static int g_redo_top = 0;

static void undo_push(int index, int was_deleted) {
    if (g_undo_top < MAX_UNDO) {
        g_undo_stack[g_undo_top].index = index;
        g_undo_stack[g_undo_top].was_deleted = was_deleted;
        g_undo_top++;
    }
    g_redo_top = 0; /* clear redo on new action */
}

/* --- Editor state --- */

typedef struct {
    char *path;
    chat_state_t state;
    int *deleted;       /* per-message deletion flag */
    int cursor;         /* current message index */
    int scroll_top;     /* first visible message */
    int dirty;          /* unsaved changes */
    char search[256];   /* current search pattern */
    regex_t search_re;  /* compiled regex */
    int search_valid;   /* regex compiled successfully */
    char status[256];   /* status bar message */
} editor_t;

/* --- Display --- */

/* Get handle colour as escape sequence string for use in sprintf */
static const char *handle_colour_str(const char *handle) {
    static char buf[NBS_STYLE_BUFSIZE];
    const nbs_style_t *style = nbs_handle_colour(handle);
    int n = nbs_style_start(style, buf, sizeof(buf));
    if (n <= 0) buf[0] = '\0';
    return buf;
}

static void render(const editor_t *ed) {
    /* Build output buffer */
    char *buf = malloc(g_term_rows * (g_term_cols + 128) + 8192);
    if (!buf) return;
    int off = 0;

    /* Move to top-left, clear screen */
    off += sprintf(buf + off, "\x1b[H\x1b[2J");

    int content_rows = g_term_rows - 3; /* header + status bar + message bar */
    int msg_count = ed->state.message_count;

    /* Header */
    off += sprintf(buf + off, "%s nbs-chat-edit: %s (%d messages)%s %s\r\n",
                   RENDER_REVERSE, ed->path, msg_count,
                   ed->dirty ? " [modified]" : "",
                   RENDER_RESET);

    /* Messages */
    for (int row = 0; row < content_rows; row++) {
        int idx = ed->scroll_top + row;
        if (idx >= msg_count) {
            off += sprintf(buf + off, "%s~%s\r\n", RENDER_DIM, RENDER_RESET);
            continue;
        }

        const chat_message_t *msg = &ed->state.messages[idx];
        int is_cursor = (idx == ed->cursor);
        int is_deleted = ed->deleted[idx];
        const char *hcol;
        /* Medic warnings get terracotta instead of palette colour */
        static char medic_buf[NBS_STYLE_BUFSIZE];
        if (strncmp(msg->handle, "[MEDIC-", 7) == 0) {
            nbs_style_start(&NBS_STYLE_MEDIC_WARNING, medic_buf,
                            sizeof(medic_buf));
            hcol = medic_buf;
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
        if (plen > (size_t)(g_term_cols - 30)) plen = (size_t)(g_term_cols - 30);
        if (plen > sizeof(preview) - 1) plen = sizeof(preview) - 1;
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

    /* Status bar — positioned explicitly to avoid overwriting content */
    int del_count = 0;
    for (int i = 0; i < msg_count; i++)
        if (ed->deleted[i]) del_count++;

    char status_left[256];
    if (del_count > 0)
        snprintf(status_left, sizeof(status_left), " %d/%d | %s%d to delete%s",
                 ed->cursor + 1, msg_count, RENDER_RED, del_count, RENDER_RESET RENDER_REVERSE);
    else
        snprintf(status_left, sizeof(status_left), " %d/%d",
                 ed->cursor + 1, msg_count);

    char status_right[256];
    if (ed->search_valid)
        snprintf(status_right, sizeof(status_right), "/%s  ", ed->search);
    else
        status_right[0] = '\0';

    off += sprintf(buf + off, "\x1b[%d;1H%s%-*s%s%s",
                   g_term_rows - 1, RENDER_REVERSE,
                   g_term_cols, status_left,
                   status_right, RENDER_RESET);

    /* Message bar */
    off += sprintf(buf + off, "\x1b[%d;1H\x1b[2K%s", g_term_rows,
                   ed->status[0] ? ed->status : " h:help d:del t:trunc u:undo w:write q:quit /:search");

    write(STDOUT_FILENO, buf, (size_t)off);
    free(buf);
}

/* --- Search --- */

static int search_forward(editor_t *ed, int from) {
    if (!ed->search_valid) return -1;
    for (int i = from; i < ed->state.message_count; i++) {
        if (regexec(&ed->search_re, ed->state.messages[i].content,
                    0, NULL, 0) == 0)
            return i;
        if (regexec(&ed->search_re, ed->state.messages[i].handle,
                    0, NULL, 0) == 0)
            return i;
    }
    return -1;
}

static int search_backward(editor_t *ed, int from) {
    if (!ed->search_valid) return -1;
    for (int i = from; i >= 0; i--) {
        if (regexec(&ed->search_re, ed->state.messages[i].content,
                    0, NULL, 0) == 0)
            return i;
        if (regexec(&ed->search_re, ed->state.messages[i].handle,
                    0, NULL, 0) == 0)
            return i;
    }
    return -1;
}

/* --- File operations --- */

static int write_changes(editor_t *ed) {
    /* Count surviving messages */
    int keep = 0;
    for (int i = 0; i < ed->state.message_count; i++)
        if (!ed->deleted[i]) keep++;

    if (keep == ed->state.message_count) {
        snprintf(ed->status, sizeof(ed->status), "No changes to write.");
        return 0;
    }

    /* Build new message array */
    chat_message_t *new_msgs = calloc((size_t)keep, sizeof(chat_message_t));
    if (!new_msgs) {
        snprintf(ed->status, sizeof(ed->status), "Error: out of memory");
        return -1;
    }

    int j = 0;
    for (int i = 0; i < ed->state.message_count; i++) {
        if (!ed->deleted[i]) {
            new_msgs[j] = ed->state.messages[i];
            /* Don't free content — we're reusing the pointers */
            j++;
        }
    }

    /* Rewrite the file: delete the old one, create fresh, re-send
     * each surviving message. Uses the existing chat_send API which
     * handles base64 encoding, header updates, and locking. */

    /* Backup path for safety */
    char backup[4200];
    snprintf(backup, sizeof(backup), "%s.edit-backup", ed->path);
    if (rename(ed->path, backup) != 0) {
        snprintf(ed->status, sizeof(ed->status), "Error: cannot backup file: %s",
                 strerror(errno));
        free(new_msgs);
        return -1;
    }

    if (chat_create(ed->path) != 0) {
        /* Restore backup */
        rename(backup, ed->path);
        snprintf(ed->status, sizeof(ed->status), "Error: cannot recreate file");
        free(new_msgs);
        return -1;
    }

    /* Re-send each surviving message */
    for (int i = 0; i < keep; i++) {
        if (chat_send(ed->path, new_msgs[i].handle,
                      new_msgs[i].content) != 0) {
            snprintf(ed->status, sizeof(ed->status),
                     "Error: write failed at message %d", i + 1);
            free(new_msgs);
            return -1;
        }
    }

    /* Success — remove backup */
    unlink(backup);

    int deleted = ed->state.message_count - keep;
    snprintf(ed->status, sizeof(ed->status),
             "Written: %d messages (%d deleted)", keep, deleted);

    /* Reload */
    free(new_msgs);
    /* Free old content for deleted messages only */
    for (int i = 0; i < ed->state.message_count; i++) {
        if (ed->deleted[i] && ed->state.messages[i].content) {
            /* Content will be freed by chat_state_free */
        }
    }
    chat_state_free(&ed->state);
    free(ed->deleted);

    if (chat_read(ed->path, &ed->state) != 0) {
        snprintf(ed->status, sizeof(ed->status), "Error: cannot reload file");
        return -1;
    }

    ed->deleted = calloc((size_t)ed->state.message_count, sizeof(int));
    if (!ed->deleted) return -1;

    ed->dirty = 0;
    g_undo_top = 0;
    g_redo_top = 0;

    if (ed->cursor >= ed->state.message_count)
        ed->cursor = ed->state.message_count - 1;
    if (ed->cursor < 0) ed->cursor = 0;

    return 0;
}

/* --- Input handling --- */

enum key {
    KEY_NONE = 0,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_PAGE_UP, KEY_PAGE_DOWN,
    KEY_HOME, KEY_END,
    KEY_CTRL_R = 18,
};

static int read_key(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return KEY_NONE;

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
                        case '5': return KEY_PAGE_UP;
                        case '6': return KEY_PAGE_DOWN;
                        case '1': return KEY_HOME;
                        case '4': return KEY_END;
                    }
                }
            }
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
        return '\x1b';
    }

    return (unsigned char)c;
}

static void prompt_search(editor_t *ed) {
    /* Show cursor, move to bottom */
    char prompt_buf[64];
    int plen = sprintf(prompt_buf, "\x1b[?25h\x1b[%d;1H\x1b[2K/", g_term_rows);
    write(STDOUT_FILENO, prompt_buf, (size_t)plen);

    /* Read search string in cooked-ish mode */
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
        snprintf(ed->status, sizeof(ed->status), "Search cancelled.");
        return;
    }

    /* Compile regex */
    if (ed->search_valid) {
        regfree(&ed->search_re);
        ed->search_valid = 0;
    }
    if (regcomp(&ed->search_re, input, REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0) {
        snprintf(ed->status, sizeof(ed->status), "Invalid regex: %s", input);
        return;
    }
    ed->search_valid = 1;
    snprintf(ed->search, sizeof(ed->search), "%s", input);

    /* Find first match from cursor */
    int found = search_forward(ed, ed->cursor);
    if (found >= 0) {
        ed->cursor = found;
        snprintf(ed->status, sizeof(ed->status), "/%s", ed->search);
    } else {
        /* Wrap around */
        found = search_forward(ed, 0);
        if (found >= 0) {
            ed->cursor = found;
            snprintf(ed->status, sizeof(ed->status),
                     "/%s (wrapped)", ed->search);
        } else {
            snprintf(ed->status, sizeof(ed->status),
                     "Pattern not found: %s", ed->search);
        }
    }
}

/* --- Main loop --- */

static void run_editor(editor_t *ed) {
    int content_rows = g_term_rows - 2;

    while (1) {
        update_term_size();
        content_rows = g_term_rows - 2;

        /* Keep cursor in scroll view */
        if (ed->cursor < ed->scroll_top)
            ed->scroll_top = ed->cursor;
        if (ed->cursor >= ed->scroll_top + content_rows)
            ed->scroll_top = ed->cursor - content_rows + 1;
        if (ed->scroll_top < 0) ed->scroll_top = 0;

        render(ed);

        int key = read_key();
        if (key == KEY_NONE) continue;

        int msg_count = ed->state.message_count;

        switch (key) {
        case KEY_UP:
        case 'k':
            if (ed->cursor > 0) ed->cursor--;
            ed->status[0] = '\0';
            break;

        case KEY_DOWN:
        case 'j':
            if (ed->cursor < msg_count - 1) ed->cursor++;
            ed->status[0] = '\0';
            break;

        case KEY_PAGE_UP:
            ed->cursor -= content_rows;
            if (ed->cursor < 0) ed->cursor = 0;
            ed->status[0] = '\0';
            break;

        case KEY_PAGE_DOWN:
            ed->cursor += content_rows;
            if (ed->cursor >= msg_count) ed->cursor = msg_count - 1;
            ed->status[0] = '\0';
            break;

        case KEY_HOME:
        case 'g':
            ed->cursor = 0;
            ed->scroll_top = 0;
            ed->status[0] = '\0';
            break;

        case KEY_END:
        case 'G':
            ed->cursor = msg_count - 1;
            ed->status[0] = '\0';
            break;

        case 'd': /* toggle delete */
            if (ed->cursor >= 0 && ed->cursor < msg_count) {
                undo_push(ed->cursor, ed->deleted[ed->cursor]);
                ed->deleted[ed->cursor] = !ed->deleted[ed->cursor];
                ed->dirty = 1;
                if (ed->cursor < msg_count - 1) ed->cursor++;
            }
            ed->status[0] = '\0';
            break;

        case 't': /* truncate from cursor */
            if (ed->cursor >= 0 && ed->cursor < msg_count) {
                for (int i = ed->cursor; i < msg_count; i++) {
                    if (!ed->deleted[i]) {
                        undo_push(i, 0);
                        ed->deleted[i] = 1;
                    }
                }
                ed->dirty = 1;
                snprintf(ed->status, sizeof(ed->status),
                         "Marked %d messages for deletion (truncate from %d)",
                         msg_count - ed->cursor, ed->cursor + 1);
            }
            break;

        case 'u': /* undo */
            if (g_undo_top > 0) {
                g_undo_top--;
                undo_entry_t *e = &g_undo_stack[g_undo_top];
                /* Push to redo before reverting */
                if (g_redo_top < MAX_UNDO) {
                    g_redo_stack[g_redo_top].index = e->index;
                    g_redo_stack[g_redo_top].was_deleted = ed->deleted[e->index];
                    g_redo_top++;
                }
                ed->deleted[e->index] = e->was_deleted;
                ed->cursor = e->index;
                /* Check if still dirty */
                ed->dirty = 0;
                for (int i = 0; i < msg_count; i++)
                    if (ed->deleted[i]) { ed->dirty = 1; break; }
                snprintf(ed->status, sizeof(ed->status), "Undo");
            } else {
                snprintf(ed->status, sizeof(ed->status), "Nothing to undo");
            }
            break;

        case KEY_CTRL_R: /* redo */
            if (g_redo_top > 0) {
                g_redo_top--;
                undo_entry_t *e = &g_redo_stack[g_redo_top];
                if (g_undo_top < MAX_UNDO) {
                    g_undo_stack[g_undo_top].index = e->index;
                    g_undo_stack[g_undo_top].was_deleted = ed->deleted[e->index];
                    g_undo_top++;
                }
                ed->deleted[e->index] = e->was_deleted;
                ed->cursor = e->index;
                ed->dirty = 0;
                for (int i = 0; i < msg_count; i++)
                    if (ed->deleted[i]) { ed->dirty = 1; break; }
                snprintf(ed->status, sizeof(ed->status), "Redo");
            } else {
                snprintf(ed->status, sizeof(ed->status), "Nothing to redo");
            }
            break;

        case '/': /* search */
            prompt_search(ed);
            break;

        case 'n': /* next match */
            if (ed->search_valid) {
                int found = search_forward(ed, ed->cursor + 1);
                if (found < 0) found = search_forward(ed, 0);
                if (found >= 0) {
                    ed->cursor = found;
                    ed->status[0] = '\0';
                } else {
                    snprintf(ed->status, sizeof(ed->status), "No more matches");
                }
            }
            break;

        case 'N': /* previous match */
            if (ed->search_valid) {
                int found = search_backward(ed, ed->cursor - 1);
                if (found < 0)
                    found = search_backward(ed, ed->state.message_count - 1);
                if (found >= 0) {
                    ed->cursor = found;
                    ed->status[0] = '\0';
                } else {
                    snprintf(ed->status, sizeof(ed->status), "No more matches");
                }
            }
            break;

        case 'w': /* write */
            write_changes(ed);
            break;

        case 'q': /* quit */
            if (ed->dirty) {
                snprintf(ed->status, sizeof(ed->status),
                         "Unsaved changes. Press Q to force quit, or w to save.");
            } else {
                return;
            }
            break;

        case 'Q': /* force quit */
            return;

        case '\r': /* Enter — view full message */
        case 'v': {
            if (ed->cursor < 0 || ed->cursor >= msg_count) break;
            const chat_message_t *msg = &ed->state.messages[ed->cursor];
            const char *hcol = handle_colour_str(msg->handle);

            char ts[64] = "";
            if (msg->timestamp > 0) {
                struct tm tm;
                gmtime_r(&msg->timestamp, &tm);
                strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
            }

            /* Render full message */
            char *vbuf = malloc(msg->content_len + 4096);
            if (!vbuf) break;
            int vo = 0;
            vo += sprintf(vbuf + vo, "\x1b[H\x1b[2J");
            vo += sprintf(vbuf + vo, "%s Message %d/%d %s\r\n\r\n",
                          RENDER_REVERSE, ed->cursor + 1, msg_count, RENDER_RESET);
            vo += sprintf(vbuf + vo, "  %sFrom:%s  %s%s%s\r\n",
                          RENDER_DIM, RENDER_RESET, hcol, msg->handle, RENDER_RESET);
            vo += sprintf(vbuf + vo, "  %sTime:%s  %s\r\n",
                          RENDER_DIM, RENDER_RESET, ts);
            if (ed->deleted[ed->cursor])
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
                    if (line_col >= g_term_cols - 1) {
                        vbuf[vo++] = '\r';
                        vbuf[vo++] = '\n';
                        line_col = 0;
                    }
                }
                p++;
            }

            vo += sprintf(vbuf + vo, "\r\n\r\n%s— Press any key to return —%s",
                          RENDER_DIM, RENDER_RESET);
            write(STDOUT_FILENO, vbuf, (size_t)vo);
            free(vbuf);
            while (read_key() == KEY_NONE) ;
            break;
        }

        case 'h': /* help */
        case '?': {
            char *hbuf = malloc(4096);
            if (!hbuf) break;
            int ho = 0;
            ho += sprintf(hbuf + ho, "\x1b[H\x1b[2J");
            ho += sprintf(hbuf + ho, "%s nbs-chat-edit — Help %s\r\n\r\n", RENDER_REVERSE, RENDER_RESET);
            ho += sprintf(hbuf + ho, "  %sNavigation%s\r\n", RENDER_BOLD, RENDER_RESET);
            ho += sprintf(hbuf + ho, "    Up/Down, j/k       One message\r\n");
            ho += sprintf(hbuf + ho, "    Page Up/Down       One screen\r\n");
            ho += sprintf(hbuf + ho, "    Home, g            First message\r\n");
            ho += sprintf(hbuf + ho, "    End, G             Last message\r\n");
            ho += sprintf(hbuf + ho, "    /                  Search (regex)\r\n");
            ho += sprintf(hbuf + ho, "    n                  Next match\r\n");
            ho += sprintf(hbuf + ho, "    N                  Previous match\r\n");
            ho += sprintf(hbuf + ho, "\r\n");
            ho += sprintf(hbuf + ho, "    Enter, v           View full message\r\n");
            ho += sprintf(hbuf + ho, "\r\n");
            ho += sprintf(hbuf + ho, "  %sEditing%s\r\n", RENDER_BOLD, RENDER_RESET);
            ho += sprintf(hbuf + ho, "    d                  Mark/unmark for deletion\r\n");
            ho += sprintf(hbuf + ho, "    t                  Truncate (delete from here to end)\r\n");
            ho += sprintf(hbuf + ho, "    u                  Undo\r\n");
            ho += sprintf(hbuf + ho, "    Ctrl-R             Redo\r\n");
            ho += sprintf(hbuf + ho, "\r\n");
            ho += sprintf(hbuf + ho, "  %sFile%s\r\n", RENDER_BOLD, RENDER_RESET);
            ho += sprintf(hbuf + ho, "    w                  Write changes\r\n");
            ho += sprintf(hbuf + ho, "    q                  Quit (warns if unsaved)\r\n");
            ho += sprintf(hbuf + ho, "    Q                  Force quit without saving\r\n");
            ho += sprintf(hbuf + ho, "\r\n");
            ho += sprintf(hbuf + ho, "  %sPress any key to return%s", RENDER_DIM, RENDER_RESET);
            write(STDOUT_FILENO, hbuf, (size_t)ho);
            free(hbuf);
            /* Wait for any key */
            while (read_key() == KEY_NONE) ;
            break;
        }

        default:
            break;
        }
    }
}

/* --- Main --- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: nbs-chat-edit <file>\n");
        return 4;
    }

    const char *path = argv[1];

    /* Resolve to absolute path */
    char abs_path[4096];
    if (path[0] != '/') {
        if (!getcwd(abs_path, sizeof(abs_path) - strlen(path) - 2)) {
            fprintf(stderr, "Error: getcwd failed\n");
            return 1;
        }
        strcat(abs_path, "/");
        strcat(abs_path, path);
    } else {
        snprintf(abs_path, sizeof(abs_path), "%s", path);
    }

    /* Initialise shared colour table */
    render_init();

    /* Read chat file */
    editor_t ed;
    memset(&ed, 0, sizeof(ed));
    ed.path = abs_path;

    if (chat_read(abs_path, &ed.state) != 0) {
        fprintf(stderr, "Error: cannot read chat file: %s\n", abs_path);
        return 1;
    }

    if (ed.state.message_count == 0) {
        fprintf(stderr, "Chat file is empty.\n");
        chat_state_free(&ed.state);
        return 0;
    }

    ed.deleted = calloc((size_t)ed.state.message_count, sizeof(int));
    if (!ed.deleted) {
        fprintf(stderr, "Error: out of memory\n");
        chat_state_free(&ed.state);
        return 1;
    }

    /* Set up terminal */
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "Error: stdin is not a terminal\n");
        free(ed.deleted);
        chat_state_free(&ed.state);
        return 1;
    }

    update_term_size();
    signal(SIGWINCH, handle_sigwinch);

    enable_raw_mode();
    atexit(disable_raw_mode);

    /* Start at the end */
    ed.cursor = ed.state.message_count - 1;
    int content_rows = g_term_rows - 2;
    ed.scroll_top = ed.cursor - content_rows + 1;
    if (ed.scroll_top < 0) ed.scroll_top = 0;

    snprintf(ed.status, sizeof(ed.status),
             "nbs-chat-edit: %d messages loaded", ed.state.message_count);

    run_editor(&ed);

    /* Cleanup */
    if (ed.search_valid) regfree(&ed.search_re);
    free(ed.deleted);
    chat_state_free(&ed.state);

    return 0;
}
