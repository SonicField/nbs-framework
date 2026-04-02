/*
 * editor.c — Interactive terminal editor for nbs-chat files.
 *
 * Thin wrapper on top of libchatview: uses the shared TUI for
 * navigation, search, and message viewing, then adds editing
 * keybindings (delete, truncate, undo/redo, write) via the
 * key handler callback.
 *
 * Usage: nbs-chat-edit <file>
 *
 * Navigation (handled by libchatview):
 *   Up/Down           One message at a time
 *   Page Up/Down      One screen at a time
 *   Home              Go to first message
 *   End               Go to last message
 *   /                 Search forward (regex)
 *   n                 Next search match
 *   N                 Previous search match
 *   Enter, v          View full message (via nbs-md-viewer)
 *
 * Editing (handled here):
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

#include "../nbs-chatview/chatview.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* Forward declarations */
static void show_editor_help(chatview_t *cv);

/* --- Editor state (passed via key handler userdata) --- */

typedef struct {
    char *path;     /* chat file path (absolute) */
} editor_ctx_t;

/* --- File operations --- */

static int write_changes(chatview_t *cv, editor_ctx_t *ctx) {
    int msg_count = cv->state.message_count;

    /* Count surviving messages */
    int keep = 0;
    for (int i = 0; i < msg_count; i++)
        if (!(cv->msg_flags[i] & CHATVIEW_MSG_DELETED)) keep++;

    if (keep == msg_count) {
        chatview_set_status(cv, "No changes to write.");
        return 0;
    }

    /* Build new message array */
    chat_message_t *new_msgs = calloc((size_t)keep, sizeof(chat_message_t));
    if (!new_msgs) {
        chatview_set_status(cv, "Error: out of memory");
        return -1;
    }

    int j = 0;
    for (int i = 0; i < msg_count; i++) {
        if (!(cv->msg_flags[i] & CHATVIEW_MSG_DELETED)) {
            new_msgs[j] = cv->state.messages[i];
            j++;
        }
    }

    /* Backup and rewrite */
    char backup[4200];
    snprintf(backup, sizeof(backup), "%s.edit-backup", ctx->path);
    if (rename(ctx->path, backup) != 0) {
        chatview_set_status(cv, "Error: cannot backup file: %s",
                            strerror(errno));
        free(new_msgs);
        return -1;
    }

    if (chat_create(ctx->path) != 0) {
        rename(backup, ctx->path);
        chatview_set_status(cv, "Error: cannot recreate file");
        free(new_msgs);
        return -1;
    }

    /* Re-send each surviving message */
    for (int i = 0; i < keep; i++) {
        if (chat_send(ctx->path, new_msgs[i].handle,
                      new_msgs[i].content) != 0) {
            chatview_set_status(cv, "Error: write failed at message %d",
                                i + 1);
            free(new_msgs);
            return -1;
        }
    }

    /* Success */
    unlink(backup);

    int deleted = msg_count - keep;
    chatview_set_status(cv, "Written: %d messages (%d deleted)",
                        keep, deleted);
    free(new_msgs);

    /* Reload file into chatview */
    if (chatview_reload(cv, ctx->path) != 0) {
        chatview_set_status(cv, "Error: cannot reload file");
        return -1;
    }

    /* Reset flags and undo state */
    cv->dirty = 0;
    g_undo_top = 0;
    g_redo_top = 0;

    if (cv->cursor >= cv->state.message_count)
        cv->cursor = cv->state.message_count - 1;
    if (cv->cursor < 0) cv->cursor = 0;

    return 0;
}

/* --- Key handler (editing layer) --- */

static int editor_key_handler(chatview_t *cv, int key, void *userdata) {
    editor_ctx_t *ctx = (editor_ctx_t *)userdata;
    int msg_count = cv->state.message_count;

    switch (key) {
    case 'd': /* toggle delete */
        if (cv->cursor >= 0 && cv->cursor < msg_count) {
            int was = cv->msg_flags[cv->cursor] & CHATVIEW_MSG_DELETED;
            undo_push(cv->cursor, was);
            cv->msg_flags[cv->cursor] ^= CHATVIEW_MSG_DELETED;
            cv->dirty = 1;
            if (cv->cursor < msg_count - 1) cv->cursor++;
        }
        cv->status[0] = '\0';
        return CHATVIEW_KEY_HANDLED;

    case 't': /* truncate from cursor */
        if (cv->cursor >= 0 && cv->cursor < msg_count) {
            for (int i = cv->cursor; i < msg_count; i++) {
                if (!(cv->msg_flags[i] & CHATVIEW_MSG_DELETED)) {
                    undo_push(i, 0);
                    cv->msg_flags[i] |= CHATVIEW_MSG_DELETED;
                }
            }
            cv->dirty = 1;
            chatview_set_status(cv,
                "Marked %d messages for deletion (truncate from %d)",
                msg_count - cv->cursor, cv->cursor + 1);
        }
        return CHATVIEW_KEY_HANDLED;

    case 'u': /* undo */
        if (g_undo_top > 0) {
            g_undo_top--;
            undo_entry_t *e = &g_undo_stack[g_undo_top];
            if (g_redo_top < MAX_UNDO) {
                g_redo_stack[g_redo_top].index = e->index;
                g_redo_stack[g_redo_top].was_deleted =
                    cv->msg_flags[e->index] & CHATVIEW_MSG_DELETED;
                g_redo_top++;
            }
            if (e->was_deleted)
                cv->msg_flags[e->index] |= CHATVIEW_MSG_DELETED;
            else
                cv->msg_flags[e->index] &= ~CHATVIEW_MSG_DELETED;
            cv->cursor = e->index;
            /* Check if still dirty */
            cv->dirty = 0;
            for (int i = 0; i < msg_count; i++)
                if (cv->msg_flags[i] & CHATVIEW_MSG_DELETED)
                    { cv->dirty = 1; break; }
            chatview_set_status(cv, "Undo");
        } else {
            chatview_set_status(cv, "Nothing to undo");
        }
        return CHATVIEW_KEY_HANDLED;

    case CHATVIEW_KEY_CTRL_R: /* redo */
        if (g_redo_top > 0) {
            g_redo_top--;
            undo_entry_t *e = &g_redo_stack[g_redo_top];
            if (g_undo_top < MAX_UNDO) {
                g_undo_stack[g_undo_top].index = e->index;
                g_undo_stack[g_undo_top].was_deleted =
                    cv->msg_flags[e->index] & CHATVIEW_MSG_DELETED;
                g_undo_top++;
            }
            if (e->was_deleted)
                cv->msg_flags[e->index] |= CHATVIEW_MSG_DELETED;
            else
                cv->msg_flags[e->index] &= ~CHATVIEW_MSG_DELETED;
            cv->cursor = e->index;
            cv->dirty = 0;
            for (int i = 0; i < msg_count; i++)
                if (cv->msg_flags[i] & CHATVIEW_MSG_DELETED)
                    { cv->dirty = 1; break; }
            chatview_set_status(cv, "Redo");
        } else {
            chatview_set_status(cv, "Nothing to redo");
        }
        return CHATVIEW_KEY_HANDLED;

    case 'w': /* write */
        write_changes(cv, ctx);
        return CHATVIEW_KEY_HANDLED;

    case 'q': /* quit */
        if (cv->dirty) {
            chatview_set_status(cv,
                "Unsaved changes. Press Q to force quit, or w to save.");
            return CHATVIEW_KEY_HANDLED;
        }
        return CHATVIEW_KEY_QUIT;

    case 'Q': /* force quit */
        return CHATVIEW_KEY_QUIT;

    case 'h': /* override help to show editor help */
    case '?':
        show_editor_help(cv);
        return CHATVIEW_KEY_HANDLED;

    default:
        return CHATVIEW_KEY_UNHANDLED;
    }
}

/* --- Editor help screen --- */

static void show_editor_help(chatview_t *cv) {
    char *hbuf = malloc(4096);
    if (!hbuf) return;
    int ho = 0;
    ho += sprintf(hbuf + ho, "\x1b[H\x1b[2J");
    ho += sprintf(hbuf + ho, "%s nbs-chat-edit — Help %s\r\n\r\n",
                  RENDER_REVERSE, RENDER_RESET);
    ho += sprintf(hbuf + ho, "  %sNavigation%s\r\n",
                  RENDER_BOLD, RENDER_RESET);
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
    ho += sprintf(hbuf + ho, "  %sEditing%s\r\n",
                  RENDER_BOLD, RENDER_RESET);
    ho += sprintf(hbuf + ho, "    d                  Mark/unmark for deletion\r\n");
    ho += sprintf(hbuf + ho, "    t                  Truncate (delete from here to end)\r\n");
    ho += sprintf(hbuf + ho, "    u                  Undo\r\n");
    ho += sprintf(hbuf + ho, "    Ctrl-R             Redo\r\n");
    ho += sprintf(hbuf + ho, "\r\n");
    ho += sprintf(hbuf + ho, "  %sFile%s\r\n",
                  RENDER_BOLD, RENDER_RESET);
    ho += sprintf(hbuf + ho, "    w                  Write changes\r\n");
    ho += sprintf(hbuf + ho, "    q                  Quit (warns if unsaved)\r\n");
    ho += sprintf(hbuf + ho, "    Q                  Force quit without saving\r\n");
    ho += sprintf(hbuf + ho, "\r\n");
    ho += sprintf(hbuf + ho, "  %sPress any key to return%s",
                  RENDER_DIM, RENDER_RESET);
    write(STDOUT_FILENO, hbuf, (size_t)ho);
    free(hbuf);
    (void)cv;
    while (chatview_read_key() == CHATVIEW_KEY_NONE) ;
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
    chat_state_t state;
    if (chat_read(abs_path, &state) != 0) {
        fprintf(stderr, "Error: cannot read chat file: %s\n", abs_path);
        return 1;
    }

    if (state.message_count == 0) {
        fprintf(stderr, "Chat file is empty.\n");
        chat_state_free(&state);
        return 0;
    }

    /* Check terminal */
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "Error: stdin is not a terminal\n");
        chat_state_free(&state);
        return 1;
    }

    /* Build title */
    char title[512];
    snprintf(title, sizeof(title), "nbs-chat-edit: %s", abs_path);

    /* Create chatview (takes ownership of state) */
    chatview_t *cv = chatview_init(&state, title);
    if (!cv) {
        fprintf(stderr, "Error: out of memory\n");
        chat_state_free(&state);
        return 1;
    }

    /* Set editor help hint */
    cv->help_hint = " h:help d:del t:trunc u:undo w:write q:quit /:search ?:search-back n/N:next/prev";

    /* Set up editor context */
    editor_ctx_t ctx = { .path = abs_path };
    chatview_set_key_handler(cv, editor_key_handler, &ctx);

    chatview_set_status(cv, "nbs-chat-edit: %d messages loaded",
                        cv->state.message_count);

    /* Run the view — blocks until quit */
    chatview_run(cv);

    /* Cleanup */
    chatview_free(cv);

    return 0;
}
