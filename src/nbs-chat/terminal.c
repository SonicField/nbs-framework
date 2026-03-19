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
#include "render.h"
#include "../nbs-common/trigger_defs.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <inttypes.h>
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

/* --- ANSI colour aliases (from render.h) --- */

#define BOLD  RENDER_BOLD
#define DIM   RENDER_DIM
#define RESET RENDER_RESET

/* --- Global state --- */

static const char *g_chat_file = NULL;
static const char *g_handle = NULL;
static int g_msg_count = 0;
static volatile sig_atomic_t g_quit = 0;
static char g_filter_handle[MAX_HANDLE_LEN] = {0};  /* empty = show all, non-empty = show only this handle */

/* Cursor row tracking for wrapped-line redraw */
static int g_cursor_row = 0;  /* Row of cursor relative to first row of input */

/* --- Input history (bash-style) --- */

#define HISTORY_MAX 50

static char *g_history[HISTORY_MAX];  /* Ring buffer of sent messages (strdup'd) */
static int g_history_count = 0;       /* Total entries stored (0..HISTORY_MAX) */
static int g_history_pos = -1;        /* Current browse position (-1 = not browsing) */

static void history_add(const char *msg) {
    if (!msg || msg[0] == '\0') return;
    /* Don't add duplicates of the most recent entry */
    if (g_history_count > 0 &&
        strcmp(g_history[g_history_count - 1], msg) == 0) return;
    if (g_history_count >= HISTORY_MAX) {
        /* Ring full — shift everything down, freeing oldest */
        free(g_history[0]);
        memmove(g_history, g_history + 1, (HISTORY_MAX - 1) * sizeof(char *));
        g_history_count = HISTORY_MAX - 1;
    }
    g_history[g_history_count] = strdup(msg);
    if (g_history[g_history_count]) g_history_count++;
}

static void history_free(void) {
    for (int i = 0; i < g_history_count; i++) {
        free(g_history[i]);
        g_history[i] = NULL;
    }
    g_history_count = 0;
    g_history_pos = -1;
}

/* history_load defined after line_state_t and line_ensure_cap (see below) */

/* --- Restart script resolution --- */

/* Find the restart script: try .nbs/bin/ first (installed projects),
 * fall back to bin/ (framework source tree). Returns 0 on success. */
static int resolve_restart_script(const char *project_root,
                                   char *out, size_t out_size) {
    int n = snprintf(out, out_size,
                     "%s/.nbs/bin/nbs-chat-terminal-restart.sh", project_root);
    if (n > 0 && (size_t)n < out_size && access(out, X_OK) == 0)
        return 0;
    n = snprintf(out, out_size,
                 "%s/bin/nbs-chat-terminal-restart.sh", project_root);
    if (n > 0 && (size_t)n < out_size && access(out, X_OK) == 0)
        return 0;
    return -1;
}

/* --- Watchdog daemon --- */

static watchdog_state_t g_watchdog;

/* Resolve project root by walking up from chat file to find .nbs/ directory */
static int resolve_project_root(const char *chat_path, char *out, size_t out_size) {
    /* Resolve to absolute path first — handles relative paths like
     * .nbs/chat/live.chat which would otherwise fail the walk-up */
    char *abs = realpath(chat_path, NULL);
    if (!abs) return -1;

    /* Start from the directory containing the chat file */
    char dir[4096];
    int n = snprintf(dir, sizeof(dir), "%s", abs);
    free(abs);
    if (n <= 0 || (size_t)n >= sizeof(dir)) return -1;

    /* Strip filename to get directory */
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else return -1; /* absolute path always has a slash */

    /* Walk up looking for .nbs/ */
    for (int i = 0; i < 10; i++) {
        /* Skip if current dir IS .nbs — that's the state dir, not the project root */
        const char *basename = strrchr(dir, '/');
        basename = basename ? basename + 1 : dir;
        if (strcmp(basename, ".nbs") == 0) {
            char *up = strrchr(dir, '/');
            if (up) *up = '\0';
            else break;
            continue;
        }

        char probe[4096 + 8];
        snprintf(probe, sizeof(probe), "%s/.nbs", dir);
        struct stat st;
        if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* dir is already absolute (resolved at top) */
            int sn = snprintf(out, out_size, "%s", dir);
            return (sn > 0 && (size_t)sn < out_size) ? 0 : -1;
        }
        /* Go up one level */
        slash = strrchr(dir, '/');
        if (!slash) break;
        *slash = '\0';
    }
    return -1;
}

/*
 * spawn_trigger_worker — Fork+exec nbs-workers to spawn an ephemeral worker.
 *
 * Used by /pythia, /shepard, /fixup, /librarian terminal commands.
 * Same action as the sidecar's periodic triggers, but manual.
 *
 * Preconditions:
 *   - role, task_desc, project_root are non-NULL
 *   - nbs-workers is findable via resolve_restart_script pattern
 *
 * Returns 0 on spawn success, -1 on failure.
 */
static int spawn_trigger_worker(const char *role, const char *skill_file,
                                 const char *task_desc,
                                 const char *project_root) {
    ASSERT_MSG(role != NULL, "spawn_trigger_worker: role is NULL");
    ASSERT_MSG(skill_file != NULL, "spawn_trigger_worker: skill_file is NULL");
    ASSERT_MSG(task_desc != NULL, "spawn_trigger_worker: task_desc is NULL");
    ASSERT_MSG(project_root != NULL, "spawn_trigger_worker: project_root is NULL");

    /* Find nbs-workers binary: try .nbs/bin/ then bin/ */
    char workers_bin[4096];
    int n = snprintf(workers_bin, sizeof(workers_bin),
                     "%s/.nbs/bin/nbs-workers", project_root);
    if (n <= 0 || (size_t)n >= sizeof(workers_bin) ||
        access(workers_bin, X_OK) != 0) {
        n = snprintf(workers_bin, sizeof(workers_bin),
                     "%s/bin/nbs-workers", project_root);
        if (n <= 0 || (size_t)n >= sizeof(workers_bin) ||
            access(workers_bin, X_OK) != 0) {
            fprintf(stderr, "warning: nbs-workers not found in %s\n",
                    project_root);
            return -1;
        }
    }

    /* Read skill file and combine with task description.
     * Skill content is embedded verbatim — no modifications. */
    /* Try project .nbs/ first, then ~/.nbs/ (global install) */
    char skill_path[4096];
    int sp = snprintf(skill_path, sizeof(skill_path),
                      "%s/.nbs/%s", project_root, skill_file);
    if (sp > 0 && (size_t)sp < sizeof(skill_path) &&
        access(skill_path, R_OK) != 0) {
        const char *home = getenv("HOME");
        if (home) {
            sp = snprintf(skill_path, sizeof(skill_path),
                          "%s/.nbs/%s", home, skill_file);
        }
    }
    char *combined = NULL;
    if (sp > 0 && (size_t)sp < sizeof(skill_path)) {
        FILE *sf = fopen(skill_path, "r");
        if (sf) {
            fseek(sf, 0, SEEK_END);
            long slen = ftell(sf);
            fseek(sf, 0, SEEK_SET);
            if (slen > 0 && slen < 64 * 1024) {
                size_t dlen = strlen(task_desc);
                size_t total = (size_t)slen + dlen + 64;
                combined = malloc(total);
                if (combined) {
                    size_t nread = fread(combined, 1, (size_t)slen, sf);
                    snprintf(combined + nread, total - nread,
                             "\n\n## Task Instructions\n\n%s", task_desc);
                }
            }
            fclose(sf);
        }
    }
    if (!combined) {
        fprintf(stderr, "warning: skill file '%s' not found, using task only\n",
                skill_path);
    }

    const char *task = combined ? combined : task_desc;

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "warning: fork for /%s failed: %s\n",
                role, strerror(errno));
        free(combined);
        return -1;
    }
    if (pid == 0) {
        /* Child: minimal cleanup before exec.
         * Do NOT setsid() or close fds — that breaks tmux session
         * communication. The CLI spawn works without any of that. */
        execl(workers_bin, "nbs-workers", "spawn", role,
              project_root, task, (char *)NULL);
        _exit(127);
    }
    /* Parent: don't wait — worker runs in background */
    free(combined);
    return 0;
}

static void *watchdog_thread_fn(void *arg) {
    watchdog_state_t *ws = (watchdog_state_t *)arg;

    while (watchdog_is_enabled(ws)) {
        sleep(WATCHDOG_POLL_INTERVAL_S);
        if (!watchdog_is_enabled(ws)) break;

        /* Derive session prefix from chat filename.
         * live.chat → "live", nn.Module.chat → "nn-Module"
         * Dots replaced with dashes (tmux rejects dots in session names). */
        char chat_tag[256];
        {
            const char *base = strrchr(ws->chat_path, '/');
            base = base ? base + 1 : ws->chat_path;
            size_t blen = strlen(base);
            /* Strip .chat suffix */
            if (blen > 5 && strcmp(base + blen - 5, ".chat") == 0)
                blen -= 5;
            if (blen >= sizeof(chat_tag)) blen = sizeof(chat_tag) - 1;
            memcpy(chat_tag, base, blen);
            chat_tag[blen] = '\0';
            /* Replace dots with dashes */
            for (size_t i = 0; i < blen; i++)
                if (chat_tag[i] == '.') chat_tag[i] = '-';
        }

        /* Count alive agent sessions for THIS chat only */
        char grep_cmd[512];
        snprintf(grep_cmd, sizeof(grep_cmd),
                 "tmux list-sessions -F '#{session_name}' 2>/dev/null | "
                 "grep -c 'nbs-.*-%s' 2>/dev/null || echo 0", chat_tag);
        FILE *fp = popen(grep_cmd, "r");
        int count = 0;
        if (fp) {
            if (fscanf(fp, "%d", &count) != 1) count = 0;
            pclose(fp);
        } else {
            fprintf(stderr, "warning: watchdog popen failed: %s\n",
                    strerror(errno));
        }

        watchdog_decision_t d = watchdog_evaluate(ws, count, time(NULL));
        if (d == WATCHDOG_RESTART) {
            char script[4096 + 64];
            if (resolve_restart_script(ws->project_root,
                                        script, sizeof(script)) == 0) {
                pid_t rpid = fork();
                if (rpid == 0) {
                    /* Child: exec restart script with project_root and chat_path */
                    execlp("bash", "bash", script,
                           ws->project_root, ws->chat_path, (char *)NULL);
                    _exit(127);
                } else if (rpid > 0) {
                    /* Wait for restart to complete before resuming polling.
                     * Without this, the watchdog fires again while the
                     * restart script is still running (digest takes minutes),
                     * spawning concurrent restarts that kill each other. */
                    int wstatus;
                    waitpid(rpid, &wstatus, 0);
                }
                /* fork failure: silently continue — next poll will retry */
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
    ASSERT_MSG(handle != NULL, "format_message: handle is NULL");
    ASSERT_MSG(content != NULL, "format_message: content is NULL");
    ASSERT_MSG(my_handle != NULL, "format_message: my_handle is NULL");

    if (strcmp(handle, my_handle) == 0) {
        render_message_own(handle, content, timestamp, stdout);
    } else {
        render_message(handle, content, timestamp, stdout);
    }
}

static void print_prompt(const char *handle) {
    ASSERT_MSG(handle != NULL, "print_prompt: handle is NULL");
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
    printf("  %s/pythia%s     Spawn pythia (trajectory & risk assessment)\n", DIM, RESET);
    printf("  %s/shepard%s    Spawn shepard (team effectiveness check)\n", DIM, RESET);
    printf("  %s/librarian%s  Spawn librarian (institutional memory search)\n", DIM, RESET);
    printf("  %s/fixup%s      Spawn fixup (diagnose & restart stalled agents)\n", DIM, RESET);
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
    ASSERT_MSG(handle != NULL, "line_redraw: handle is NULL");
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

/* Load a history entry into the line editor.
 * Placed here because it needs line_state_t, line_ensure_cap, and line_redraw. */
static void history_load(line_state_t *ls, int pos, const char *handle) {
    if (pos < 0 || pos >= g_history_count) return;
    size_t hlen = strlen(g_history[pos]);
    line_ensure_cap(ls, hlen + 1);
    memcpy(ls->buf, g_history[pos], hlen + 1);
    ls->len = hlen;
    ls->cursor = hlen;
    line_redraw(ls, handle);
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
    ASSERT_MSG(handle != NULL, "handle_escape_input: handle is NULL");
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
        case 'A': /* Up arrow — history browse (only when buffer is empty or already browsing) */
            if (g_history_count > 0 && (ls->len == 0 || g_history_pos >= 0)) {
                if (g_history_pos < 0) {
                    /* Start browsing from most recent */
                    g_history_pos = g_history_count - 1;
                } else if (g_history_pos > 0) {
                    g_history_pos--;
                }
                history_load(ls, g_history_pos, handle);
            }
            break;
        case 'B': /* Down arrow — history forward (only when browsing) */
            if (g_history_pos >= 0) {
                if (g_history_pos < g_history_count - 1) {
                    g_history_pos++;
                    history_load(ls, g_history_pos, handle);
                } else {
                    /* Past end of history — clear to empty */
                    g_history_pos = -1;
                    line_state_reset(ls);
                    line_redraw(ls, handle);
                }
            }
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
    ASSERT_MSG(handle != NULL, "poll_and_display: handle is NULL");
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

    int send_rc = chat_send(g_chat_file, g_handle, msg);

    if (send_rc == 0) {
        history_add(msg);
        g_history_pos = -1;
    }

    if (send_rc != 0) {
        int saved_errno = errno;
        fprintf(stderr, "warning: chat_send failed: %s (errno=%d)\n",
                strerror(saved_errno), saved_errno);
        return -1;
    }

    /* Do NOT increment g_msg_count here. Let poll_and_display read the
     * actual file count on the next poll. If we increment here and another
     * agent also sent a message between our chat_send and the next poll,
     * our count is 1 behind reality and that agent's message is permanently
     * skipped — the desync bug. poll_and_display will see our message as
     * "new", skip it via the handle filter (line 715), and advance the
     * count correctly. */

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
     * to prevent other users from reading the draft message.
     * Use TMPDIR if set (allows user-private temp directories),
     * falling back to /tmp. */
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0') tmpdir = "/tmp";
    char tmppath[4096];
    int tsn = snprintf(tmppath, sizeof(tmppath),
                       "%s/nbs-chat-edit.XXXXXX", tmpdir);
    if (tsn <= 0 || (size_t)tsn >= sizeof(tmppath)) return NULL;
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
    printf("  nbs-chat-terminal <file> <handle> [--restart]\n\n");
    printf("  <file>      Path to chat file (must exist)\n");
    printf("  <handle>    Your display name in the chat\n");
    printf("  --restart   Start/restart the agent team immediately\n\n");
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

    /* Check for --restart flag */
    int restart_immediately = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--restart") == 0) {
            restart_immediately = 1;
        }
    }

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
        } else {
            /* Postcondition: verify the terminal driver accepted our flags.
             * POSIX permits tcsetattr to silently ignore unsupported flags. */
            struct termios verify;
            if (tcgetattr(STDIN_FILENO, &verify) == 0) {
                if ((verify.c_lflag & (ECHO | ICANON)) != 0) {
                    fprintf(stderr, "warning: tcsetattr postcondition failed: "
                            "ECHO or ICANON still set (lflag=0x%lx)\n",
                            (unsigned long)verify.c_lflag);
                }
                if ((verify.c_lflag & ISIG) == 0) {
                    fprintf(stderr, "warning: tcsetattr postcondition failed: "
                            "ISIG not set (lflag=0x%lx) — Ctrl-C may not work\n",
                            (unsigned long)verify.c_lflag);
                }
            }
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
        /* --restart: run restart script immediately, no cooldown */
        if (restart_immediately) {
            char script[4096 + 64];
            if (resolve_restart_script(wd_project_root,
                                        script, sizeof(script)) == 0) {
                printf("%sRestarting team...%s\n", DIM, RESET);
                pid_t rpid = fork();
                if (rpid == 0) {
                    execlp("bash", "bash", script,
                           wd_project_root, g_chat_file, (char *)NULL);
                    _exit(127);
                } else if (rpid > 0) {
                    int wstatus;
                    waitpid(rpid, &wstatus, 0);
                    printf("%sTeam restart complete.%s\n", DIM, RESET);
                }
            }
        }

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
            g_history_pos = -1;  /* Exit history browse mode */

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
                if (watchdog_is_enabled(&g_watchdog)) {
                    do_send("@team Good work — time to wrap up. "
                            "Please commit any uncommitted changes, "
                            "post a final session summary, and shut down cleanly.");
                    watchdog_disable(&g_watchdog);
                    printf("  %sWatchdog disabled. Team will not be auto-restarted.%s\n",
                           DIM, RESET);
                } else {
                    printf("  %sWatchdog already disabled.%s\n", DIM, RESET);
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* /restart — manual team restart (bypasses rate limit) */
            if (strcmp(edit.buf, "/restart") == 0) {
                if (!watchdog_is_enabled(&g_watchdog)) {
                    printf("  %sWatchdog not initialised — cannot restart.%s\n",
                           DIM, RESET);
                } else {
                    printf("  %sTriggering manual restart...%s\n", DIM, RESET);
                    char rscript[4096 + 64];
                    if (resolve_restart_script(g_watchdog.project_root,
                                               rscript, sizeof(rscript)) == 0) {
                        pid_t rpid = fork();
                        if (rpid == 0) {
                            /* Child: exec restart script */
                            execlp("bash", "bash", rscript,
                                   g_watchdog.project_root,
                                   g_watchdog.chat_path, (char *)NULL);
                            _exit(127);
                        } else if (rpid < 0) {
                            fprintf(stderr, "warning: fork for /restart failed: %s\n",
                                    strerror(errno));
                        }
                        /* Parent: do not wait — restart runs in background */
                    }
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* Trigger commands: /pythia, /shepard, /librarian, /fixup */
            if (strcmp(edit.buf, "/pythia") == 0 ||
                strcmp(edit.buf, "/shepard") == 0 ||
                strcmp(edit.buf, "/librarian") == 0 ||
                strcmp(edit.buf, "/fixup") == 0) {
                const char *role = edit.buf + 1;  /* skip the '/' */
                const char *desc = NULL;
                const char *skill = NULL;
                if (strcmp(role, "pythia") == 0) {
                    desc = TRIGGER_DESC_PYTHIA;
                    skill = TRIGGER_SKILL_PYTHIA;
                } else if (strcmp(role, "shepard") == 0) {
                    desc = TRIGGER_DESC_SHEPARD;
                    skill = TRIGGER_SKILL_SHEPARD;
                } else if (strcmp(role, "librarian") == 0) {
                    desc = TRIGGER_DESC_LIBRARIAN;
                    skill = TRIGGER_SKILL_LIBRARIAN;
                } else if (strcmp(role, "fixup") == 0) {
                    desc = TRIGGER_DESC_FIXUP;
                    skill = TRIGGER_SKILL_FIXUP;
                }

                if (!watchdog_is_enabled(&g_watchdog)) {
                    printf("  %sWatchdog not initialised — no project root.%s\n",
                           DIM, RESET);
                } else if (desc && skill) {
                    printf("  %sSpawning %s worker...%s\n", DIM, role, RESET);
                    if (spawn_trigger_worker(role, skill, desc,
                                              g_watchdog.project_root) == 0) {
                        printf("  %s%s spawned (will post to chat when done).%s\n",
                               DIM, role, RESET);
                    } else {
                        printf("  %sFailed to spawn %s.%s\n", DIM, role, RESET);
                    }
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
    history_free();
    if (have_termios) {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) != 0) {
            fprintf(stderr, "warning: tcsetattr(final restore) failed: %s\n",
                    strerror(errno));
        }
    }
    printf("\n%sLeft chat.%s\n", DIM, RESET);

    return 0;
}
