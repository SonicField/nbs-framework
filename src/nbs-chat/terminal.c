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
#include "handle_styles.h"
#include "../nbs-chatview/chatview.h"
#include "../nbs-dashboard/dashboard.h"
#include "../nbs-common/trigger_defs.h"
#include "../nbs-common/nbs_helper_check.h"
#include "../nbs-common/nbs_mention.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
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
#include <pty.h>

#include "watchdog.h"

/* --- Configuration --- */

#define POLL_INTERVAL_MS 1500  /* Background message poll interval */
#define MAX_CHILD_PIPES  8     /* Max concurrent captured child processes */

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
static char g_mention_handle[MAX_HANDLE_LEN] = {0}; /* empty = no mention filter, non-empty = show only messages mentioning this handle */

/* Cursor row tracking for wrapped-line redraw */
static int g_cursor_row = 0;  /* Row of cursor relative to first row of input */

/* /file command: remember last directory across invocations */
static char g_file_last_dir[PATH_MAX] = {0};

/* /bash command: remember working directory across invocations */
static char g_bash_cwd[PATH_MAX] = {0};

/* Scrollback mode: 0 = live (normal), >0 = N screen lines back from end */
static int g_scrollback_offset = 0;

/* Forward declaration — defined near poll_and_display */
static void scrollback_render(void);

/* Half-page scroll amount based on terminal height */
static int scroll_half_page(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 4)
        return ws.ws_row / 2;
    return 12;
}

/*
 * Count visible columns in a string, skipping ANSI escape sequences.
 * Stops at newline or end of string. Returns the number of visible chars
 * and advances *pp past the consumed input (including the newline if hit).
 */
static int visible_line_width(const char **pp) {
    int w = 0;
    const char *p = *pp;
    while (*p && *p != '\n') {
        if (*p == '\033') {
            p++;
            if (*p == '[') {
                p++;
                while (*p && !((*p >= 'A' && *p <= 'Z') ||
                               (*p >= 'a' && *p <= 'z')))
                    p++;
                if (*p) p++;
            }
            continue;
        }
        w++;
        p++;
    }
    if (*p == '\n') p++;
    *pp = p;
    return w;
}

/*
 * Split rendered output into screen lines accounting for terminal wrapping.
 * Each output line in the buffer may wrap across multiple screen lines.
 * Returns an array of line pointers (into buf) and sets *out_count.
 * Caller must free the returned array (but not the strings — they point
 * into buf which the caller owns).
 */
#define SCROLLBACK_MAX_LINES 131072

static char **split_screen_lines(char *buf, int term_width,
                                 int *out_count) {
    char **lines = calloc(SCROLLBACK_MAX_LINES, sizeof(char *));
    if (!lines) { *out_count = 0; return NULL; }
    int count = 0;

    char *p = buf;
    while (*p && count < SCROLLBACK_MAX_LINES) {
        char *line_start = p;

        /* Measure visible width of this logical line */
        const char *scan = p;
        int vis_width = visible_line_width(&scan);
        /* scan now points past the \n (or at \0) */

        if (vis_width <= term_width || term_width <= 0) {
            /* Fits on one screen line */
            lines[count++] = line_start;
            p = (char *)scan;
        } else {
            /* Line wraps — split into chunks of term_width visible
             * chars. Walk through counting visible chars, skipping
             * ANSI escapes, and start a new screen line every
             * term_width visible chars. */
            const char *cp = line_start;
            /* Find the newline (or end of string) */
            const char *nl = cp;
            while (*nl && *nl != '\n') nl++;

            while (cp < nl && count < SCROLLBACK_MAX_LINES) {
                lines[count++] = (char *)cp;
                int col = 0;
                while (cp < nl) {
                    if (*cp == '\033') {
                        cp++;
                        if (*cp == '[') {
                            cp++;
                            while (*cp && !((*cp >= 'A' && *cp <= 'Z')
                                   || (*cp >= 'a' && *cp <= 'z')))
                                cp++;
                            if (*cp) cp++;
                        }
                        continue;
                    }
                    col++;
                    cp++;
                    if (col >= term_width) break;
                }
            }
            /* Advance past the newline */
            if (*nl == '\n') nl++;
            p = (char *)nl;
        }
    }
    *out_count = count;
    return lines;
}

/* @mention highlighting is always enabled */

/* Auto-repair: set while repair is in flight, cleared when
 * skipped_count drops to 0 (repair completed successfully). */
static int g_repair_running = 0;

/* --- Child pipe capture (INFO lines) --- */

typedef struct {
    int fd;              /* pipe read end, -1 = unused */
    char label[32];      /* e.g. "restart", "team-check" */
    char line_buf[512];  /* partial line accumulator */
    size_t line_len;
} child_pipe_t;

static child_pipe_t g_child_pipes[MAX_CHILD_PIPES];
static int g_child_pipe_count = 0;

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
    if (g_history[g_history_count]) {
        g_history_count++;
    } else {
        fprintf(stderr, "warning: history_add: strdup failed, history entry dropped\n");
    }
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

/* --- Command autocompletion --- */

static const char *g_commands[] = {
    "/bash", "/broadcast", "/browse", "/dashboard", "/digest", "/edit", "/exit",
    "/file", "/filter", "/fixup", "/health", "/help",
    "/kick", "/librarian", "/mention",
    "/paste", "/pause", "/pythia",
    "/redraw", "/restart", "/resume",
    "/search", "/shepard", "/shutdown", "/sidecar",
    "/unfilter", "/unmention",
    NULL
};

/*
 * find_completion — Find unique command prefix match.
 * Returns the full command string if exactly one command matches,
 * NULL if zero or multiple commands match.
 */
static const char *find_completion(const char *buf, size_t len) {
    if (len < 2 || buf[0] != '/') return NULL;
    const char *match = NULL;
    for (const char **cmd = g_commands; *cmd; cmd++) {
        if (strncmp(*cmd, buf, len) == 0) {
            if (match) return NULL; /* ambiguous — two or more matches */
            match = *cmd;
        }
    }
    /* Don't suggest if already complete */
    if (match && strlen(match) == len) return NULL;
    return match;
}

/*
 * is_known_command — Return 1 if `buf` starts with one of the slash
 * commands in `g_commands` (exact match on the first whitespace-
 * delimited word). Used to suppress accidental typos like `/dsa` from
 * being sent to chat as plain text — the user's mistake should produce
 * an out-of-chat warning, not pollute the conversation.
 */
static int is_known_command(const char *buf) {
    if (!buf || buf[0] != '/') return 0;
    size_t wlen = 0;
    while (buf[wlen] && buf[wlen] != ' ' && buf[wlen] != '\t')
        wlen++;
    for (const char **cmd = g_commands; *cmd; cmd++) {
        size_t clen = strlen(*cmd);
        if (clen == wlen && memcmp(buf, *cmd, clen) == 0)
            return 1;
    }
    return 0;
}

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
    ASSERT_MSG(chat_path != NULL, "resolve_project_root: chat_path is NULL");
    ASSERT_MSG(out != NULL, "resolve_project_root: out is NULL");
    ASSERT_MSG(out_size > 0, "resolve_project_root: out_size must be positive, got %zu", out_size);
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
 * oracle_worker_active — Check whether a worker for `role` currently has a
 * live nbs-ts session. Mirror of triggers.c::worker_session_active.
 *
 * Defends against duplicate spawns when the user types the same slash
 * command twice in quick succession, or when the slash command races with
 * the sidecar's periodic trigger. Returns 1 if a worker is alive, 0 if not
 * or on error (fail-open).
 */
static int oracle_worker_active(const char *role) {
    char prefix_arg[128];
    int n = snprintf(prefix_arg, sizeof(prefix_arg),
                     "--name=nbs-%s-worker-", role);
    if (n <= 0 || (size_t)n >= sizeof(prefix_arg))
        return 0;

    int pipefd[2];
    if (pipe(pipefd) < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return 0; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(pipefd[1]);
        execlp("nbs-ts", "nbs-ts", "list", prefix_arg, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);

    char out[8192];
    size_t total = 0;
    ssize_t r;
    while (total < sizeof(out) - 1 &&
           (r = read(pipefd[0], out + total, sizeof(out) - 1 - total)) > 0) {
        total += (size_t)r;
    }
    out[total] = '\0';
    close(pipefd[0]);
    int wstatus;
    waitpid(pid, &wstatus, 0);
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0)
        return 0;

    /* Each line: handle\tstatus\tname\t... — match status==alive */
    char *saveptr = NULL;
    for (char *line = strtok_r(out, "\n", &saveptr);
         line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        char *status = tab + 1;
        char *tab2 = strchr(status, '\t');
        size_t status_len = tab2 ? (size_t)(tab2 - status) : strlen(status);
        if (status_len == 5 && memcmp(status, "alive", 5) == 0)
            return 1;
    }
    return 0;
}

/*
 * touch_oracle_timestamp — Atomically update <project_root>/.nbs/<role>-last-run
 * to the current time. This blocks the sidecar's periodic trigger from
 * firing again until the periodic interval has elapsed, eliminating the
 * manual+periodic doubling that produced the original duplicate-spawn bug.
 *
 * Best-effort: failure is silent. Worst case is one extra spawn.
 */
static void touch_oracle_timestamp(const char *role,
                                   const char *project_root) {
    char path[4096], tmp_path[4096];
    int n = snprintf(path, sizeof(path), "%s/.nbs/%s-last-run",
                     project_root, role);
    if (n <= 0 || (size_t)n >= sizeof(path)) return;
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.XXXXXX", path);
    if (tn <= 0 || (size_t)tn >= sizeof(tmp_path)) return;

    int tfd = mkstemp(tmp_path);
    if (tfd < 0) return;
    FILE *f = fdopen(tfd, "w");
    if (!f) { close(tfd); unlink(tmp_path); return; }
    fprintf(f, "%lld\n", (long long)time(NULL));
    if (fclose(f) != 0) { unlink(tmp_path); return; }
    if (rename(tmp_path, path) != 0) unlink(tmp_path);
}

/*
 * spawn_trigger_worker — Fork+exec nbs-workers spawn.
 *
 * Single source of truth for worker lifecycle. Double-fork to
 * avoid zombie processes.
 *
 * Returns 0 on spawn success, -1 on failure, 1 if skipped because a
 * worker for this role is already alive (caller should report to user).
 */
static int spawn_trigger_worker(const char *role, const char *skill_file,
                                 const char *task_desc,
                                 const char *project_root) {
    ASSERT_MSG(role != NULL, "spawn_trigger_worker: role is NULL");
    ASSERT_MSG(skill_file != NULL, "spawn_trigger_worker: skill_file is NULL");
    ASSERT_MSG(task_desc != NULL, "spawn_trigger_worker: task_desc is NULL");
    ASSERT_MSG(project_root != NULL, "spawn_trigger_worker: project_root is NULL");

    /* Dedup: if a worker for this role is already alive, refuse to spawn
     * a duplicate. This catches both rapid-fire slash commands and the
     * race against the sidecar periodic trigger. */
    if (oracle_worker_active(role))
        return 1;

    /* Find nbs-workers: try .nbs/bin/ then bin/ */
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

    /* Build --skill=FILE flag */
    char skill_flag[4096];
    snprintf(skill_flag, sizeof(skill_flag), "--skill=%s", skill_file);

    /* Update timestamp BEFORE fork — blocks periodic from firing again
     * for the periodic interval. Mirrors what trigger_periodic_spawn does
     * under its lock. Done before fork because the parent's filesystem
     * write is what other sidecars observe. */
    touch_oracle_timestamp(role, project_root);

    /* Double-fork to avoid zombie processes */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "warning: fork for /%s failed: %s\n",
                role, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            /* Grandchild: exec nbs-workers spawn.
             * Redirect stdout/stderr to /dev/null — oracle output
             * goes to chat, not the terminal. Without this, nbs-workers
             * prints diagnostics that stomp on the user's input area. */
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            execl(workers_bin, "nbs-workers", "spawn", role,
                  project_root, skill_flag, task_desc, (char *)NULL);
            _exit(127);
        }
        _exit(pid2 < 0 ? 127 : 0);
    }
    /* Parent: reap intermediate child (exits immediately) */
    int wstatus;
    waitpid(pid, &wstatus, 0);
    return 0;
}

/* Watchdog auto-restart thread removed. Crash recovery is handled by
 * /fixup (manual or sidecar-triggered). The watchdog state machine
 * (watchdog.c) is retained for /pause and /resume state tracking. */

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

/* --- Mention matching for /mention filter --- */

static int content_mentions(const char *content, const char *handle) {
    size_t hlen = strlen(handle);
    const char *p = content;
    while ((p = strstr(p, "@")) != NULL) {
        if (p > content && nbs_is_email_prefix_char((unsigned char)p[-1])) {
            p++;
            continue;
        }
        if (strncmp(p + 1, handle, hlen) == 0) {
            char after = p[1 + hlen];
            if (after == '\0' || !nbs_is_handle_char((unsigned char)after)) {
                return 1;
            }
        }
        p++;
    }
    return 0;
}

/* --- Display functions --- */

static void format_message_to(FILE *out, const char *handle,
                              const char *content,
                              const char *my_handle, time_t timestamp) {
    ASSERT_MSG(handle != NULL, "format_message: handle is NULL");
    ASSERT_MSG(content != NULL, "format_message: content is NULL");
    ASSERT_MSG(my_handle != NULL, "format_message: my_handle is NULL");

    const nbs_style_t *bracket_style = handle_style_lookup(handle);
    if (strcmp(handle, my_handle) == 0) {
        render_message_own(handle, content, timestamp, out);
    } else if (bracket_style) {
        render_message_bracket(handle, content, timestamp, bracket_style, out);
    } else {
        render_message(handle, content, timestamp, out);
    }
}

static void format_message(const char *handle, const char *content,
                           const char *my_handle, time_t timestamp) {
    format_message_to(stdout, handle, content, my_handle, timestamp);
}

/* --- Browse mode poll callback --- */

/*
 * Poll callback for /browse mode — checks for new messages.
 *
 * Ownership contract:
 *   - chat_read() allocates new_state (caller owns)
 *   - If new messages: chatview_update() takes ownership of new_state
 *     (frees old state, adopts new). Caller must NOT free.
 *   - If no new messages: caller frees new_state.
 */
static void browse_poll_cb(chatview_t *cv, void *userdata) {
    const char *chat_file = (const char *)userdata;
    chat_state_t new_state;
    if (chat_read(chat_file, &new_state) != 0) return;
    if (new_state.message_count > cv->state.message_count) {
        chatview_update(cv, &new_state); /* takes ownership */
    } else {
        chat_state_free(&new_state);     /* no change, free locally */
    }
}

static void print_prompt(const char *handle) {
    ASSERT_MSG(handle != NULL, "print_prompt: handle is NULL");
    printf("%s%s%s>%s ", RENDER_REVERSE, BOLD, handle, RESET);
    fflush(stdout);
}

static void print_help(void) {
    printf("\n");
    printf("%sCommands:%s\n", BOLD, RESET);
    printf("  %s/bash%s        Interactive shell (exit to return) or /bash <cmd>\n", DIM, RESET);
    printf("  %s/browse%s      Scroll through full chat history\n", DIM, RESET);
    printf("  %s/dashboard%s   Live team dashboard — agents, sidecars, activity\n", DIM, RESET);
    printf("  %s/file%s        Browse files (e.g. /file src/) — remembers last directory\n", DIM, RESET);
    printf("  %s/edit%s       Open $EDITOR to compose a multi-line message\n", DIM, RESET);
    printf("  %s/paste%s      Full-screen editor for pasting and tweaking (ESC to send)\n", DIM, RESET);
    printf("  %s/search%s     Search message history (e.g. /search parser)\n", DIM, RESET);
    printf("  %s/filter%s     Show only one participant (e.g. /filter pythia)\n", DIM, RESET);
    printf("  %s/unfilter%s   Return to showing all messages\n", DIM, RESET);
    printf("  %s/mention%s    Show messages mentioning @handle (e.g. /mention alex)\n", DIM, RESET);
    printf("  %s/unmention%s  Clear mention filter\n", DIM, RESET);
    printf("  %s/pause%s      Pause team — agents keep context, stop receiving work\n", DIM, RESET);
    printf("  %s/resume%s     Resume paused team\n", DIM, RESET);
    printf("  %s/shutdown%s   Announce shutdown, wait 10s, kill all agents\n", DIM, RESET);
    printf("  %s/restart%s    Manually restart the agent team\n", DIM, RESET);
    printf("  %s/pythia%s     Spawn pythia (trajectory & risk assessment)\n", DIM, RESET);
    printf("  %s/shepard%s    Spawn shepard (team effectiveness check)\n", DIM, RESET);
    printf("  %s/librarian%s  Spawn librarian (institutional memory search)\n", DIM, RESET);
    printf("  %s/fixup%s      Spawn fixup (diagnose & restart stalled agents)\n", DIM, RESET);
    printf("  %s/digest%s     Spawn chatdigest (extract learnings from chat)\n", DIM, RESET);
    printf("  %s/broadcast%s  Send text directly to all agent terminals (e.g. /broadcast stop)\n", DIM, RESET);
    printf("  %s/kick%s       Hard restart a single agent (e.g. /kick scribe)\n", DIM, RESET);
    printf("  %s/sidecar%s    Restart all sidecars (e.g. /sidecar testkeeper for just one)\n", DIM, RESET);
    printf("  %s/health%s     Report team health (agents and sidecars)\n", DIM, RESET);
    printf("  %s/redraw%s     Clear screen and repaint chat\n", DIM, RESET);
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

    /* Ghost autocompletion: show the remaining suffix of a matching
     * command in dim grey after the typed text. The ghost text is
     * purely visual — cursor positioning ignores it. */
    int ghost_len = 0;
    const char *completion = find_completion(ls->buf, ls->len);
    if (completion && ls->cursor == ls->len) {
        const char *suffix = completion + ls->len;
        ghost_len = (int)strlen(suffix);
        printf("%s%s%s", DIM, suffix, RESET);
    }

    /* Calculate where the cursor needs to be vs where it is now.
     * After printing, the cursor is at the end of the content
     * PLUS any ghost text. Both positions are measured in characters
     * from the start.
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

    int end_abs = prompt_vlen + (int)ls->len + ghost_len;
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

/* --- INFO line rendering --- */

/*
 * info_line_emit — Render an INFO line above the prompt without disrupting input.
 *
 * Same pattern as poll_and_display: clear input area, print content, redraw.
 */
static void info_line_emit(line_state_t *ls, const char *handle,
                           const char *label, const char *text) {
    /* Move cursor up to the first row of the input area */
    if (g_cursor_row > 0) {
        printf("\033[%dA", g_cursor_row);
    }
    printf("\r\033[J");

    /* Print INFO line in dim */
    printf("  %sINFO> [%s] %s%s\n", DIM, label, text, RESET);

    /* Restore prompt + input */
    g_cursor_row = 0;
    line_redraw(ls, handle);
}

/*
 * child_pipe_register — Track a pipe fd for captured child output.
 * Sets O_NONBLOCK so reads in the poll loop don't block.
 */
static void child_pipe_register(int fd, const char *label) {
    ASSERT_MSG(fd >= 0, "child_pipe_register: fd is negative: %d", fd);
    ASSERT_MSG(label != NULL, "child_pipe_register: label is NULL");
    ASSERT_MSG(g_child_pipe_count < MAX_CHILD_PIPES,
               "child_pipe_register: too many child pipes (%d)", g_child_pipe_count);

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    child_pipe_t *cp = &g_child_pipes[g_child_pipe_count++];
    cp->fd = fd;
    snprintf(cp->label, sizeof(cp->label), "%s", label);
    cp->line_buf[0] = '\0';
    cp->line_len = 0;
}

/*
 * child_pipe_drain — Read available bytes from a child pipe, split on
 * newlines, and emit complete lines as INFO lines. Accumulates partial
 * lines. On EOF: flush any partial line and close the fd.
 */
static void child_pipe_drain(child_pipe_t *cp, line_state_t *ls,
                             const char *handle) {
    ASSERT_MSG(cp != NULL, "child_pipe_drain: cp is NULL");
    ASSERT_MSG(cp->fd >= 0, "child_pipe_drain: fd is closed");

    char buf[256];
    for (;;) {
        ssize_t n = read(cp->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            /* Read error — treat as EOF */
            break;
        }
        if (n == 0) break; /* EOF */

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                /* Complete line — emit it */
                cp->line_buf[cp->line_len] = '\0';
                if (cp->line_len > 0) {
                    info_line_emit(ls, handle, cp->label, cp->line_buf);
                }
                cp->line_len = 0;
            } else if (cp->line_len < sizeof(cp->line_buf) - 1) {
                cp->line_buf[cp->line_len++] = buf[i];
            }
            /* else: line too long, silently truncate */
        }
    }

    /* EOF: flush partial line if any */
    if (cp->line_len > 0) {
        cp->line_buf[cp->line_len] = '\0';
        info_line_emit(ls, handle, cp->label, cp->line_buf);
        cp->line_len = 0;
    }
    close(cp->fd);
    cp->fd = -1;
}

/*
 * child_pipe_compact — Remove closed entries (fd == -1) from the array.
 */
static void child_pipe_compact(void) {
    int dst = 0;
    for (int src = 0; src < g_child_pipe_count; src++) {
        if (g_child_pipes[src].fd >= 0) {
            if (dst != src) {
                g_child_pipes[dst] = g_child_pipes[src];
            }
            dst++;
        }
    }
    g_child_pipe_count = dst;
}

/*
 * spawn_with_capture — Fork+exec a command, capturing stdout+stderr
 * via a pipe registered for INFO line rendering.
 *
 * Single fork (not double) because we hold the pipe. The child becomes
 * a zombie until reaped by waitpid in the poll loop timeout branch.
 *
 * Returns the child pid on success, -1 on failure.
 */
static pid_t spawn_with_capture(const char *label, const char *argv[]) {
    ASSERT_MSG(label != NULL, "spawn_with_capture: label is NULL");
    ASSERT_MSG(argv != NULL, "spawn_with_capture: argv is NULL");

    if (g_child_pipe_count >= MAX_CHILD_PIPES) {
        fprintf(stderr, "warning: too many child pipes, cannot capture %s\n",
                label);
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        fprintf(stderr, "warning: pipe() failed for %s: %s\n",
                label, strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        fprintf(stderr, "warning: fork() failed for %s: %s\n",
                label, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect stdout+stderr to pipe write end */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent: close write end, register read end */
    close(pipefd[1]);
    child_pipe_register(pipefd[0], label);
    return pid;
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
            } else if (esc->param == 5) {
                /* Page Up — enter or continue scrollback (half page) */
                if (g_scrollback_offset == 0)
                    g_scrollback_offset = scroll_half_page();
                else
                    g_scrollback_offset += scroll_half_page();
                scrollback_render();
            } else if (esc->param == 6) {
                /* Page Down — scroll forward in scrollback (half page) */
                if (g_scrollback_offset > 0) {
                    g_scrollback_offset -= scroll_half_page();
                    if (g_scrollback_offset < 0)
                        g_scrollback_offset = 0;
                    if (g_scrollback_offset == 0) {
                        /* Back to live — redraw normally */
                        printf("\033[2J\033[H");
                        {
                            chat_state_t rs;
                            if (chat_read(g_chat_file, &rs) == 0) {
                                int s = rs.message_count - 50;
                                if (s < 0) s = 0;
                                for (int ri = s;
                                     ri < rs.message_count; ri++) {
                                    if (g_filter_handle[0] != '\0' &&
                                        strcmp(rs.messages[ri].handle,
                                               g_filter_handle) != 0)
                                        continue;
                                    if (g_mention_handle[0] != '\0' &&
                                        !content_mentions(
                                            rs.messages[ri].content,
                                            g_mention_handle))
                                        continue;
                                    format_message(
                                        rs.messages[ri].handle,
                                        rs.messages[ri].content,
                                        g_handle,
                                        rs.messages[ri].timestamp);
                                }
                                g_msg_count = rs.message_count;
                                chat_state_free(&rs);
                            }
                        }
                        g_cursor_row = 0;
                        line_redraw(ls, handle);
                    } else {
                        scrollback_render();
                    }
                }
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

/* --- Paste mode: full-screen multi-line editor --- */

/*
 * Cursor navigation helpers for multi-line buffers.
 * The buffer is a flat char array with \n separating lines.
 */

/* Find the start of the line containing position pos. */
static size_t paste_line_start(const char *buf, size_t pos) {
    if (pos == 0) return 0;
    size_t i = pos;
    while (i > 0 && buf[i - 1] != '\n') i--;
    return i;
}

/* Find the end of the line containing position pos (points to \n or len). */
static size_t paste_line_end(const char *buf, size_t pos, size_t len) {
    size_t i = pos;
    while (i < len && buf[i] != '\n') i++;
    return i;
}

/* Column of cursor within its line. */
static size_t paste_col(const char *buf, size_t pos) {
    return pos - paste_line_start(buf, pos);
}

/* Count lines in buffer. */
static int paste_line_count(const char *buf, size_t len) {
    int n = 1;
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n') n++;
    return n;
}

/* Which line number (0-based) is cursor on? */
static int paste_cursor_line(const char *buf, size_t pos) {
    int n = 0;
    for (size_t i = 0; i < pos; i++)
        if (buf[i] == '\n') n++;
    return n;
}

/*
 * Screen-row accounting for wrapped lines.
 *
 * A buffer line of length L occupies ceil(max(L,1) / tw) screen rows.
 * The cursor's screen row within the buffer is the sum of screen rows
 * for all lines before the cursor's line, plus the cursor's column
 * divided by tw.
 */

/* Screen rows consumed by a buffer line of length line_len. */
static int paste_wrap_rows(int line_len, int tw) {
    if (line_len == 0) return 1;
    return (line_len + tw - 1) / tw;
}

/* Total screen rows for the entire buffer. */
__attribute__((unused))
static int paste_total_screen_rows(const char *buf, size_t len, int tw) {
    int rows = 0;
    int col = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            rows += paste_wrap_rows(col, tw);
            col = 0;
        } else {
            col++;
        }
    }
    rows += paste_wrap_rows(col, tw);
    return rows;
}

/* Screen row (0-based) of the cursor position. */
static int paste_cursor_screen_row(const char *buf, size_t cursor, int tw) {
    int row = 0;
    int col = 0;
    for (size_t i = 0; i < cursor; i++) {
        if (buf[i] == '\n') {
            row += paste_wrap_rows(col, tw);
            col = 0;
        } else {
            col++;
        }
    }
    row += col / tw; /* wrap rows within current line */
    return row;
}

/* Full-screen render of the paste buffer with line wrapping. */
static void paste_redraw(const line_state_t *ls) {
    int tw = get_terminal_width();
    struct winsize ws;
    int th = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
        th = ws.ws_row;

    int content_rows = th - 2; /* header + status bar */
    int cur_line = paste_cursor_line(ls->buf, ls->cursor);
    int cur_col = (int)paste_col(ls->buf, ls->cursor);
    int total_lines = paste_line_count(ls->buf, ls->len);
    int cursor_screen_row = paste_cursor_screen_row(ls->buf, ls->cursor, tw);

    /* Scroll in screen rows — keep cursor visible */
    static int scroll_top = 0;
    if (cursor_screen_row < scroll_top)
        scroll_top = cursor_screen_row;
    if (cursor_screen_row >= scroll_top + content_rows)
        scroll_top = cursor_screen_row - content_rows + 1;

    /* Move to top-left, clear screen */
    printf("\033[H\033[2J");

    /* Header */
    printf("\033[7m /paste — ESC to send, Ctrl-C to cancel \033[0m\n");

    /* Build a flat view of screen rows from the buffer.
     * Walk the buffer, tracking which screen row we're on.
     * Print only screen rows in [scroll_top, scroll_top + content_rows). */
    int screen_row = 0;
    int rows_printed = 0;
    size_t i = 0;

    while (i <= ls->len && rows_printed < content_rows) {
        /* Find the next buffer line */
        size_t line_start = i;
        while (i < ls->len && ls->buf[i] != '\n') i++;
        int line_len = (int)(i - line_start);
        int wrap_count = paste_wrap_rows(line_len, tw);

        for (int w = 0; w < wrap_count; w++) {
            if (screen_row >= scroll_top && screen_row < scroll_top + content_rows) {
                int seg_start = w * tw;
                int seg_len = line_len - seg_start;
                if (seg_len > tw) seg_len = tw;
                if (seg_len > 0)
                    fwrite(ls->buf + line_start + seg_start, 1, (size_t)seg_len, stdout);
                printf("\r\n");
                rows_printed++;
            }
            screen_row++;
        }

        /* Skip past \n */
        if (i < ls->len && ls->buf[i] == '\n') i++;
        else if (i >= ls->len) break;
    }

    /* Fill remaining rows with ~ */
    while (rows_printed < content_rows) {
        printf("%s~%s\r\n", DIM, RESET);
        rows_printed++;
    }

    /* Status bar */
    printf("\033[7m Line %d/%d  Col %d  (%zu bytes) \033[0m",
           cur_line + 1, total_lines, cur_col + 1, ls->len);

    /* Position cursor: screen_row relative to scroll_top */
    int vis_row = cursor_screen_row - scroll_top + 2; /* +1 header, +1 for 1-based */
    int vis_col = (cur_col % tw) + 1;
    printf("\033[%d;%dH", vis_row, vis_col);

    fflush(stdout);
}

/*
 * paste_mode — Full-screen multi-line editor.
 *
 * Returns 1 if the user submitted (ESC), 0 if cancelled (Ctrl-C).
 * The buffer in ls contains the message on return (caller sends it).
 */
static int paste_mode(line_state_t *ls) {
    /* Disable ISIG so Ctrl-C arrives as byte 0x03 instead of
     * generating SIGINT (which kills the terminal). Restore on exit. */
    struct termios paste_save, paste_raw;
    tcgetattr(STDIN_FILENO, &paste_save);
    paste_raw = paste_save;
    paste_raw.c_lflag &= ~(unsigned)ISIG;
    tcsetattr(STDIN_FILENO, TCSANOW, &paste_raw);

    paste_redraw(ls);

    while (1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            if (n == 0) {
                tcsetattr(STDIN_FILENO, TCSANOW, &paste_save);
                return 0; /* EOF */
            }
            if (errno == EINTR || errno == EAGAIN) continue;
            tcsetattr(STDIN_FILENO, TCSANOW, &paste_save);
            return 0;
        }

        /* ESC — check for escape sequence vs bare ESC (submit) */
        if (c == 0x1B) {
            /* Try to read next byte with short timeout.
             * If nothing follows, it's a bare ESC (submit).
             * If '[' follows, it's an escape sequence. */
            struct termios cur, tmp;
            tcgetattr(STDIN_FILENO, &cur);
            tmp = cur;
            tmp.c_cc[VMIN] = 0;
            tmp.c_cc[VTIME] = 1; /* 100ms timeout */
            tcsetattr(STDIN_FILENO, TCSANOW, &tmp);

            char seq;
            ssize_t nr = read(STDIN_FILENO, &seq, 1);
            tcsetattr(STDIN_FILENO, TCSANOW, &cur);

            if (nr <= 0) {
                /* Bare ESC — submit */
                tcsetattr(STDIN_FILENO, TCSANOW, &paste_save);
                return 1;
            }

            if (seq == '[') {
                /* CSI sequence — read the final byte */
                char fin;
                /* Accumulate numeric param */
                int param = -1;
                while (1) {
                    if (read(STDIN_FILENO, &fin, 1) != 1) break;
                    if (fin >= '0' && fin <= '9') {
                        if (param < 0) param = 0;
                        param = param * 10 + (fin - '0');
                        continue;
                    }
                    break;
                }

                switch (fin) {
                case 'A': /* Up arrow */
                {
                    size_t ls_start = paste_line_start(ls->buf, ls->cursor);
                    if (ls_start == 0) break; /* already on first line */
                    size_t col = ls->cursor - ls_start;
                    /* Find start of previous line */
                    size_t prev_end = ls_start - 1; /* the \n */
                    size_t prev_start = paste_line_start(ls->buf, prev_end);
                    size_t prev_len = prev_end - prev_start;
                    ls->cursor = prev_start + (col < prev_len ? col : prev_len);
                    break;
                }
                case 'B': /* Down arrow */
                {
                    size_t ls_end = paste_line_end(ls->buf, ls->cursor, ls->len);
                    if (ls_end >= ls->len) break; /* already on last line */
                    size_t col = paste_col(ls->buf, ls->cursor);
                    size_t next_start = ls_end + 1; /* skip the \n */
                    size_t next_end = paste_line_end(ls->buf, next_start, ls->len);
                    size_t next_len = next_end - next_start;
                    ls->cursor = next_start + (col < next_len ? col : next_len);
                    break;
                }
                case 'C': /* Right arrow */
                    line_move_right(ls);
                    break;
                case 'D': /* Left arrow */
                    line_move_left(ls);
                    break;
                case 'H': /* Home */
                    ls->cursor = paste_line_start(ls->buf, ls->cursor);
                    break;
                case 'F': /* End */
                    ls->cursor = paste_line_end(ls->buf, ls->cursor, ls->len);
                    break;
                case '~':
                    if (param == 3) /* Delete */
                        line_delete_forward(ls);
                    else if (param == 1) /* Home (alt) */
                        ls->cursor = paste_line_start(ls->buf, ls->cursor);
                    else if (param == 4) /* End (alt) */
                        ls->cursor = paste_line_end(ls->buf, ls->cursor, ls->len);
                    else if (param == 5) { /* Page Up */
                        struct winsize pws;
                        int rows = 20;
                        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &pws) == 0)
                            rows = pws.ws_row - 4;
                        for (int i = 0; i < rows; i++) {
                            size_t s = paste_line_start(ls->buf, ls->cursor);
                            if (s == 0) break;
                            size_t col = ls->cursor - s;
                            size_t pe = s - 1;
                            size_t ps = paste_line_start(ls->buf, pe);
                            size_t pl = pe - ps;
                            ls->cursor = ps + (col < pl ? col : pl);
                        }
                    } else if (param == 6) { /* Page Down */
                        struct winsize pws;
                        int rows = 20;
                        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &pws) == 0)
                            rows = pws.ws_row - 4;
                        for (int i = 0; i < rows; i++) {
                            size_t e = paste_line_end(ls->buf, ls->cursor, ls->len);
                            if (e >= ls->len) break;
                            size_t col = paste_col(ls->buf, ls->cursor);
                            size_t ns = e + 1;
                            size_t ne = paste_line_end(ls->buf, ns, ls->len);
                            size_t nl = ne - ns;
                            ls->cursor = ns + (col < nl ? col : nl);
                        }
                    }
                    break;
                }
            }
            /* Other ESC sequences (Alt+key) — ignore */
            paste_redraw(ls);
            continue;
        }

        /* Ctrl-C — cancel */
        if (c == 3) {
            tcsetattr(STDIN_FILENO, TCSANOW, &paste_save);
            return 0;
        }

        /* Enter — insert newline */
        if (c == '\n' || c == '\r') {
            line_insert_char(ls, '\n');
            paste_redraw(ls);
            continue;
        }

        /* Backspace */
        if (c == 127 || c == 8) {
            if (ls->cursor > 0) {
                line_delete_back(ls);
            }
            paste_redraw(ls);
            continue;
        }

        /* Tab — insert literal tab (or spaces) */
        if (c == '\t') {
            line_insert_char(ls, ' ');
            line_insert_char(ls, ' ');
            line_insert_char(ls, ' ');
            line_insert_char(ls, ' ');
            paste_redraw(ls);
            continue;
        }

        /* Ignore other control chars */
        if (c < 32) continue;

        /* Printable character */
        line_insert_char(ls, c);
        paste_redraw(ls);
    }
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

/* --- Scrollback mode --- */

/*
 * scrollback_render — Redraw the screen showing historical messages.
 *
 * Renders all filtered messages through the real render functions into
 * a memory buffer, splits into screen lines accounting for terminal
 * wrapping, then displays the correct slice based on g_scrollback_offset.
 * No estimation — uses actual rendered output.
 */
static void scrollback_render(void) {
    chat_state_t state;
    if (chat_read(g_chat_file, &state) < 0) return;

    g_msg_count = state.message_count;

    struct winsize ws;
    int rows = 24, cols = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) rows = ws.ws_row;
        if (ws.ws_col > 0) cols = ws.ws_col;
    }
    int content_rows = rows - 1; /* status bar */
    if (content_rows < 1) content_rows = 1;

    /* Render all filtered messages into a memory buffer */
    char *render_buf = NULL;
    size_t render_size = 0;
    FILE *mem = open_memstream(&render_buf, &render_size);
    if (!mem) { chat_state_free(&state); return; }

    for (int i = 0; i < state.message_count; i++) {
        if (g_filter_handle[0] != '\0' &&
            strcmp(state.messages[i].handle, g_filter_handle) != 0)
            continue;
        if (g_mention_handle[0] != '\0' &&
            !content_mentions(state.messages[i].content,
                              g_mention_handle))
            continue;
        format_message_to(mem, state.messages[i].handle,
                         state.messages[i].content, g_handle,
                         state.messages[i].timestamp);
    }
    fclose(mem);
    chat_state_free(&state);

    if (!render_buf) return;

    /* Split into screen lines with wrapping */
    int line_count = 0;
    char **lines = split_screen_lines(render_buf, cols, &line_count);
    if (!lines) { free(render_buf); return; }

    /* Clamp offset */
    int max_offset = line_count - content_rows;
    if (max_offset < 0) max_offset = 0;
    if (g_scrollback_offset > max_offset)
        g_scrollback_offset = max_offset;
    if (g_scrollback_offset < 0)
        g_scrollback_offset = 0;

    /* Calculate which screen lines to display */
    int first_line = line_count - content_rows - g_scrollback_offset;
    if (first_line < 0) first_line = 0;
    int last_line = first_line + content_rows;
    if (last_line > line_count) last_line = line_count;

    printf("\033[H\033[2J");

    /* Print each screen line. Lines in the array point into render_buf
     * and are delimited by the next entry (or end of buffer). We need
     * to print each line followed by a reset and newline. */
    for (int i = first_line; i < last_line; i++) {
        char *start = lines[i];
        char *end;
        if (i + 1 < line_count)
            end = lines[i + 1];
        else
            end = render_buf + render_size;

        /* Write the line content, stripping trailing \n */
        size_t len = (size_t)(end - start);
        while (len > 0 && (start[len - 1] == '\n' ||
                           start[len - 1] == '\0'))
            len--;
        fwrite(start, 1, len, stdout);
        printf("\033[0m\r\n");
    }

    /* Status bar */
    int pct = line_count > 0 ?
              (first_line + content_rows) * 100 / line_count : 100;
    if (pct > 100) pct = 100;
    char status[256];
    snprintf(status, sizeof(status),
             " SCROLLBACK  %d%%  (PgUp/PgDn scroll, ESC to return)",
             pct);
    printf("\033[%d;1H\033[7m%-*s\033[0m", rows, cols, status);
    fflush(stdout);

    free(lines);
    free(render_buf);
}

/* --- Non-destructive message display --- */

/*
 * Check for new messages and display them without disrupting user input.
 * Only clears and redraws when messages from others actually arrive.
 */
/*
 * poll_and_display — Check for new messages and display them.
 *
 * Returns 1 if the display was redrawn (prompt already printed via
 * line_redraw), 0 if nothing changed (caller must print prompt).
 */
static int poll_and_display(line_state_t *ls, const char *handle) {
    ASSERT_MSG(handle != NULL, "poll_and_display: handle is NULL");
    ASSERT_MSG(g_chat_file != NULL, "poll_and_display: g_chat_file is NULL");
    ASSERT_MSG(g_msg_count >= 0,
               "poll_and_display: g_msg_count negative: %d", g_msg_count);

    chat_state_t state;
    if (chat_read(g_chat_file, &state) < 0) return 0;

    /* Auto-repair: skipped_count > 0 means real corruption exists
     * (chat_read silently skips space-padded repair artefacts without
     * counting them).  Trigger repair once; clear when count drops to 0. */
    if (state.skipped_count > 0 && !g_repair_running) {
        g_repair_running = 1;
        char count_str[64];
        snprintf(count_str, sizeof(count_str),
                 "%d corrupt line(s) detected, running auto-repair...",
                 state.skipped_count);
        info_line_emit(ls, handle, "repair", count_str);
        const char *argv[] = {
            "nbs-chat-repair", g_chat_file, NULL
        };
        spawn_with_capture("repair", argv);
    } else if (state.skipped_count == 0) {
        g_repair_running = 0;
    }

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
        return 1;
    }

    if (state.message_count <= g_msg_count) {
        chat_state_free(&state);
        return 0;
    }

    /* Check if any new messages should be displayed */
    int has_displayable = 0;
    for (int i = g_msg_count; i < state.message_count; i++) {
        if (strcmp(state.messages[i].handle, g_handle) == 0) continue;
        if (g_filter_handle[0] != '\0' &&
            strcmp(state.messages[i].handle, g_filter_handle) != 0) continue;
        if (g_mention_handle[0] != '\0' &&
            !content_mentions(state.messages[i].content, g_mention_handle)) continue;
        has_displayable = 1;
        break;
    }

    if (!has_displayable) {
        g_msg_count = state.message_count;
        chat_state_free(&state);
        return 0;
    }

    /* Clear the current input line (may span multiple visual rows) */
    if (g_cursor_row > 0) {
        printf("\033[%dA", g_cursor_row);
    }
    printf("\r\033[J");

    /* Display new messages (filtered if g_filter_handle/g_mention_handle is set) */
    for (int i = g_msg_count; i < state.message_count; i++) {
        if (strcmp(state.messages[i].handle, g_handle) == 0) continue;
        if (g_filter_handle[0] != '\0' &&
            strcmp(state.messages[i].handle, g_filter_handle) != 0) continue;
        if (g_mention_handle[0] != '\0' &&
            !content_mentions(state.messages[i].content, g_mention_handle)) continue;
        format_message(state.messages[i].handle,
                      state.messages[i].content, g_handle,
                      state.messages[i].timestamp);
    }

    g_msg_count = state.message_count;
    chat_state_free(&state);

    /* Restore prompt and user input — cursor starts from fresh line */
    g_cursor_row = 0;
    line_redraw(ls, handle);
    return 1;
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

static void send_and_display(line_state_t *ls, int input_rows) {
    ASSERT_MSG(ls != NULL, "send_and_display: ls is NULL");
    ASSERT_MSG(ls->len > 0, "send_and_display: called with empty buffer");

    /* Clear the input area — move up past the \n and all wrapped rows,
     * then clear from there to end of screen. input_rows is the
     * g_cursor_row value from before Enter was pressed. */
    int up = input_rows + 1;  /* +1 for the \n printed by Enter handler */
    if (up > 0) {
        printf("\033[%dA", up);
    }
    printf("\r\033[J");

    if (do_send(ls->buf) != 0) {
        printf("  %s(send failed)%s\n", DIM, RESET);
    } else {
        format_message(g_handle, ls->buf, g_handle, time(NULL));
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
    printf("  nbs-chat-terminal <file> <handle> [--restart] [--goal-file=PATH]\n\n");
    printf("  <file>      Path to chat file (must exist)\n");
    printf("  <handle>    Your display name in the chat\n");
    printf("  --restart           Start/restart the agent team immediately\n");
    printf("  --goal-file=PATH    Inject file contents into chat as session goal\n");
    printf("                      (posted as your handle, before restart/digest)\n\n");
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

    /* Check for --restart and --goal-file flags */
    int restart_immediately = 0;
    const char *goal_file_path = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--restart") == 0) {
            restart_immediately = 1;
        } else if (strncmp(argv[i], "--goal-file=", 12) == 0) {
            goal_file_path = argv[i] + 12;
        } else if (strcmp(argv[i], "--highlight-mention") == 0) {
            /* Accepted for backwards compatibility, now always on */
        } else {
            fprintf(stderr, "Error: unknown flag '%s'\n", argv[i]);
            print_usage();
            return 4;
        }
    }

    /* @mention highlighting is always enabled */
    render_set_highlight_handle(g_handle);

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

    /* --goal-file: validate, read, and inject BEFORE any restart or digest.
     *
     * This is deliberately early and deliberately paranoid. If the goal file
     * cannot be read, we abort before touching the chat file or launching
     * any agents. A half-injected goal with a running team is worse than
     * no injection at all.
     *
     * Checks:
     *   1. File exists and is a regular file (not a directory, pipe, etc.)
     *   2. File is non-empty (empty goal is a mistake, not a feature)
     *   3. File is not too large (>64KB is probably wrong file)
     *   4. File is readable (permissions)
     *   5. Read succeeds completely (no partial reads)
     *   6. chat_send succeeds (chat file not corrupted)
     */
    if (goal_file_path != NULL) {
        /* 1. Exists and is regular file */
        struct stat goal_st;
        if (stat(goal_file_path, &goal_st) != 0) {
            fprintf(stderr, "Error: Goal file not found: %s\n",
                    goal_file_path);
            return 1;
        }
        if (!S_ISREG(goal_st.st_mode)) {
            fprintf(stderr, "Error: Goal file is not a regular file: %s\n",
                    goal_file_path);
            return 1;
        }

        /* 2. Non-empty */
        if (goal_st.st_size == 0) {
            fprintf(stderr, "Error: Goal file is empty: %s\n",
                    goal_file_path);
            return 1;
        }

        /* 3. Not too large (64KB limit — goal files are short documents) */
        if (goal_st.st_size > 65536) {
            fprintf(stderr, "Error: Goal file too large (%lld bytes, max 64KB): %s\n",
                    (long long)goal_st.st_size, goal_file_path);
            return 1;
        }

        /* 4. Readable */
        FILE *gf = fopen(goal_file_path, "r");
        if (gf == NULL) {
            fprintf(stderr, "Error: Cannot open goal file: %s (%s)\n",
                    goal_file_path, strerror(errno));
            return 1;
        }

        /* 5. Read completely */
        size_t goal_size = (size_t)goal_st.st_size;
        char *goal_content = malloc(goal_size + 1);
        if (goal_content == NULL) {
            fprintf(stderr, "Error: Failed to allocate %zu bytes for goal file\n",
                    goal_size + 1);
            fclose(gf);
            return 1;
        }

        size_t nread = fread(goal_content, 1, goal_size, gf);
        fclose(gf);

        if (nread != goal_size) {
            fprintf(stderr, "Error: Short read on goal file: got %zu of %zu bytes\n",
                    nread, goal_size);
            free(goal_content);
            return 1;
        }
        goal_content[nread] = '\0';

        /* Verify content is not binary garbage (check for null bytes) */
        if (strlen(goal_content) != nread) {
            /* strlen hit a null byte before end — likely binary file */
            fprintf(stderr, "Error: Goal file appears to be binary "
                    "(contains null bytes): %s\n", goal_file_path);
            free(goal_content);
            return 1;
        }

        /* 6. Inject into chat */
        printf("Injecting goal file: %s (%zu bytes)\n", goal_file_path, nread);

        int send_rc = chat_send(g_chat_file, g_handle, goal_content);
        free(goal_content);

        if (send_rc != 0) {
            fprintf(stderr, "Error: Failed to inject goal file into chat "
                    "(chat_send returned %d, errno=%d: %s)\n",
                    send_rc, errno, strerror(errno));
            fprintf(stderr, "Chat file may be corrupted or locked. "
                    "Aborting — no agents launched.\n");
            return 1;
        }

        printf("Goal injected as '%s'. Agents will see this on startup.\n",
               g_handle);
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

    /* --restart safety check: if agents are already running, confirm
     * before killing them. Must happen before raw mode (needs canonical
     * input for Y/N prompt).
     * Count THIS project's agents via nbs-ts list --name=<tag>
     * (project-scoped by session name, not global). */
    if (restart_immediately) {
        int running = 0;

        /* Derive chat_tag from g_chat_file basename:
         * poem.chat → "poem", nn.Module.chat → "nn-Module" */
        char restart_tag[256] = {0};
        {
            const char *base = strrchr(g_chat_file, '/');
            base = base ? base + 1 : g_chat_file;
            size_t blen = strlen(base);
            if (blen > 5 && strcmp(base + blen - 5, ".chat") == 0)
                blen -= 5;
            if (blen >= sizeof(restart_tag)) blen = sizeof(restart_tag) - 1;
            memcpy(restart_tag, base, blen);
            restart_tag[blen] = '\0';
            for (size_t i = 0; i < blen; i++)
                if (restart_tag[i] == '.') restart_tag[i] = '-';
        }

        if (restart_tag[0] != '\0') {
            /* popen is safe here — no threads have been started yet. */
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "nbs-ts list --name=%s 2>/dev/null | grep -c alive || echo 0",
                     restart_tag);
            FILE *fp = popen(cmd, "r");
            if (fp) {
                if (fscanf(fp, "%d", &running) != 1) running = 0;
                pclose(fp);
            }
        }

        if (running > 0) {
            printf("%s%d agent session(s) currently running. "
                   "Restart will kill them all.%s\n", BOLD, running, RESET);
            printf("Continue? [y/N] ");
            fflush(stdout);
            int ch = getchar();
            if (ch != 'y' && ch != 'Y') {
                printf("Cancelled.\n");
                return 0;
            }
            /* consume trailing newline */
            if (ch != '\n') { int c; while ((c = getchar()) != '\n' && c != EOF); }
        }
    }

    /* Check nbs-ts-helper — warn if not running */
    if (!helper_is_running()) {
        printf("%s", BOLD);
        helper_warn_if_not_running(stdout, 1);
        printf("%s\n", RESET);
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
        /* --restart: run restart script immediately, no cooldown.
         * Uses spawn_with_capture + synchronous drain so output
         * renders as INFO lines, matching the /restart command. */
        if (restart_immediately) {
            char script[4096 + 64];
            if (resolve_restart_script(wd_project_root,
                                        script, sizeof(script)) == 0) {
                info_line_emit(&edit, g_handle, "restart",
                               "Restarting team...");
                const char *restart_argv[] = {
                    "bash", script,
                    wd_project_root, g_chat_file, NULL
                };
                pid_t rpid = spawn_with_capture("restart", restart_argv);
                if (rpid > 0) {
                    /* Drain pipe synchronously — we're blocking anyway */
                    int wstatus;
                    while (waitpid(rpid, &wstatus, WNOHANG) == 0) {
                        /* Drain any available child pipe data */
                        for (int i = 0; i < g_child_pipe_count; i++) {
                            if (g_child_pipes[i].fd >= 0) {
                                child_pipe_drain(&g_child_pipes[i],
                                                 &edit, g_handle);
                            }
                        }
                        child_pipe_compact();
                        usleep(100000); /* 100ms between polls */
                    }
                    /* Final drain after child exits */
                    for (int i = 0; i < g_child_pipe_count; i++) {
                        if (g_child_pipes[i].fd >= 0) {
                            child_pipe_drain(&g_child_pipes[i],
                                             &edit, g_handle);
                        }
                    }
                    child_pipe_compact();
                    info_line_emit(&edit, g_handle, "restart",
                                   "Team restart complete.");
                }
            }
        }

        /* Init watchdog state — holds project_root and chat_path needed
         * by oracle commands, /pause, /resume, /kick, /health, /restart.
         * No auto-restart thread — crash recovery is via /fixup. */
        watchdog_init(&g_watchdog, g_chat_file, wd_project_root);
    } else {
        fprintf(stderr, "warning: could not resolve project root from %s "
                        "— watchdog disabled\n", g_chat_file);
    }

    time_t last_reaper_check_t = time(NULL);
    time_t last_sidecar_watchdog_t = time(NULL);

    /* --- Event loop --- */
    while (!g_quit) {
        struct pollfd pfds[1 + MAX_CHILD_PIPES];
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        int nfds = 1;
        for (int i = 0; i < g_child_pipe_count; i++) {
            if (g_child_pipes[i].fd >= 0) {
                pfds[nfds].fd = g_child_pipes[i].fd;
                pfds[nfds].events = POLLIN;
                nfds++;
            }
        }

        int ready = poll(pfds, (nfds_t)nfds, POLL_INTERVAL_MS);

        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Process child pipe events first (drain data, emit INFO lines) */
        if (ready > 0) {
            int cp_idx = 0;
            for (int i = 1; i < nfds; i++) {
                /* Find corresponding child_pipe_t — pfds[i] maps to
                 * the cp_idx'th active pipe (fd >= 0) */
                while (cp_idx < g_child_pipe_count &&
                       g_child_pipes[cp_idx].fd < 0)
                    cp_idx++;
                if (cp_idx >= g_child_pipe_count) break;

                if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    child_pipe_drain(&g_child_pipes[cp_idx], &edit, g_handle);
                }
                cp_idx++;
            }
            child_pipe_compact();
        }

        /* Timeout branch: poll for new messages + reap zombies */
        if (ready == 0) {
            if (g_scrollback_offset == 0)
                poll_and_display(&edit, g_handle);

            /* Reap zombie children from spawn_with_capture */
            {
                int wstatus;
                while (waitpid(-1, &wstatus, WNOHANG) > 0)
                    ; /* reap all available */
            }

            /* Oracle reaper check — every 10s, kill oracles that posted */
            if (g_watchdog.project_root[0] != '\0' &&
                watchdog_is_enabled(&g_watchdog)) {
                time_t reaper_now = time(NULL);
                if ((reaper_now - last_reaper_check_t) >= 10) {
                    last_reaper_check_t = reaper_now;
                    char reaper_bin[4096];
                    int rn = snprintf(reaper_bin, sizeof(reaper_bin),
                                     "%s/.nbs/bin/nbs-oracle-reaper",
                                     g_watchdog.project_root);
                    if (rn > 0 && (size_t)rn < sizeof(reaper_bin) &&
                        access(reaper_bin, X_OK) != 0) {
                        rn = snprintf(reaper_bin, sizeof(reaper_bin),
                                     "%s/bin/nbs-oracle-reaper",
                                     g_watchdog.project_root);
                    }
                    if (rn > 0 && (size_t)rn < sizeof(reaper_bin) &&
                        access(reaper_bin, X_OK) == 0) {
                        pid_t rpid = fork();
                        if (rpid == 0) {
                            pid_t rpid2 = fork();
                            if (rpid2 == 0) {
                                execl(reaper_bin, "nbs-oracle-reaper",
                                      "check", g_watchdog.project_root,
                                      (char *)NULL);
                                _exit(127);
                            }
                            _exit(rpid2 < 0 ? 127 : 0);
                        }
                        if (rpid > 0) waitpid(rpid, NULL, 0);
                    }
                }
            }

            /* Sidecar watchdog — every 601s, respawn missing sidecars.
             * 601 is prime to avoid synchronisation with other periodic
             * checks. Only runs when not paused and project root is set. */
            if (g_watchdog.project_root[0] != '\0') {
                time_t sc_wd_now = time(NULL);
                if ((sc_wd_now - last_sidecar_watchdog_t) >= 601) {
                    last_sidecar_watchdog_t = sc_wd_now;

                    /* Check pause file */
                    char pause_chk[4200];
                    snprintf(pause_chk, sizeof(pause_chk),
                             "%s/.nbs/control-pause",
                             g_watchdog.project_root);
                    struct stat pchk_st;
                    if (stat(pause_chk, &pchk_st) != 0) {
                        /* Not paused — run sidecar respawn */
                        char sc_wd_bin[4096];
                        int swn = snprintf(sc_wd_bin, sizeof(sc_wd_bin),
                                     "%s/.nbs/bin/nbs-sidecar-restart",
                                     g_watchdog.project_root);
                        if (swn <= 0 || (size_t)swn >= sizeof(sc_wd_bin)
                            || access(sc_wd_bin, X_OK) != 0) {
                            swn = snprintf(sc_wd_bin, sizeof(sc_wd_bin),
                                     "%s/bin/nbs-sidecar-restart",
                                     g_watchdog.project_root);
                        }
                        if (swn > 0 && (size_t)swn < sizeof(sc_wd_bin)
                            && access(sc_wd_bin, X_OK) == 0) {
                            char sc_wd_root[4200];
                            snprintf(sc_wd_root, sizeof(sc_wd_root),
                                     "--root=%s",
                                     g_watchdog.project_root);
                            pid_t swpid = fork();
                            if (swpid == 0) {
                                pid_t swpid2 = fork();
                                if (swpid2 == 0) {
                                    /* Redirect stdout/stderr to /dev/null
                                     * — respawn output is logged by the
                                     * sidecar-restart script itself */
                                    int devnull = open("/dev/null",
                                                       O_WRONLY);
                                    if (devnull >= 0) {
                                        dup2(devnull, STDOUT_FILENO);
                                        dup2(devnull, STDERR_FILENO);
                                        close(devnull);
                                    }
                                    execl(sc_wd_bin,
                                          "nbs-sidecar-restart",
                                          "--respawn", sc_wd_root,
                                          (char *)NULL);
                                    _exit(127);
                                }
                                _exit(swpid2 < 0 ? 127 : 0);
                            }
                            if (swpid > 0) waitpid(swpid, NULL, 0);
                        }
                    }
                }
            }

            continue;
        }

        /* Read input if available — prioritise POLLIN over POLLHUP
         * because on pipes both can be set simultaneously when data
         * remains in the buffer after the write end closes. */
        if (!(pfds[0].revents & POLLIN)) {
            /* No data to read — check for hangup/error */
            if (pfds[0].revents & (POLLHUP | POLLERR)) {
                if (edit.len > 0) {
                    printf("\n");
                    send_and_display(&edit, 0);
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
                    send_and_display(&edit, 0);
                }
                break;
            }
            if (errno != EINTR && errno != EAGAIN) break;
            continue;
        }

        /* Scrollback mode: only PageUp, PageDown, and ESC are active.
         * All other input is suppressed until we return to live. */
        if (g_scrollback_offset > 0) {
            if (c == 0x1B) {
                /* Read next char with short timeout to distinguish
                 * bare ESC from escape sequence */
                struct termios tc_cur, tc_tmp;
                tcgetattr(STDIN_FILENO, &tc_cur);
                tc_tmp = tc_cur;
                tc_tmp.c_cc[VMIN] = 0;
                tc_tmp.c_cc[VTIME] = 1; /* 100ms */
                tcsetattr(STDIN_FILENO, TCSANOW, &tc_tmp);

                char seq0;
                ssize_t snr = read(STDIN_FILENO, &seq0, 1);
                if (snr <= 0) {
                    /* Bare ESC — exit scrollback */
                    tcsetattr(STDIN_FILENO, TCSANOW, &tc_cur);
                    g_scrollback_offset = 0;
                    printf("\033[2J\033[H");
                    {
                        chat_state_t rs;
                        if (chat_read(g_chat_file, &rs) == 0) {
                            int s = rs.message_count - 50;
                            if (s < 0) s = 0;
                            for (int ri = s;
                                 ri < rs.message_count; ri++) {
                                if (g_filter_handle[0] != '\0' &&
                                    strcmp(rs.messages[ri].handle,
                                           g_filter_handle) != 0)
                                    continue;
                                if (g_mention_handle[0] != '\0' &&
                                    !content_mentions(
                                        rs.messages[ri].content,
                                        g_mention_handle))
                                    continue;
                                format_message(
                                    rs.messages[ri].handle,
                                    rs.messages[ri].content,
                                    g_handle,
                                    rs.messages[ri].timestamp);
                            }
                            g_msg_count = rs.message_count;
                            chat_state_free(&rs);
                        }
                    }
                    g_cursor_row = 0;
                    line_redraw(&edit, g_handle);
                    continue;
                }
                if (seq0 == '[') {
                    /* CSI sequence — read param + final */
                    int param = -1;
                    char fc;
                    while (read(STDIN_FILENO, &fc, 1) == 1) {
                        if (fc >= '0' && fc <= '9') {
                            if (param < 0) param = 0;
                            param = param * 10 + (fc - '0');
                        } else {
                            break; /* final char */
                        }
                    }
                    tcsetattr(STDIN_FILENO, TCSANOW, &tc_cur);
                    if (fc == '~' && param == 5) {
                        /* Page Up — half page */
                        g_scrollback_offset += scroll_half_page();
                        scrollback_render();
                    } else if (fc == '~' && param == 6) {
                        /* Page Down — half page */
                        g_scrollback_offset -= scroll_half_page();
                        if (g_scrollback_offset <= 0) {
                            g_scrollback_offset = 0;
                            printf("\033[2J\033[H");
                            {
                                chat_state_t rs;
                                if (chat_read(g_chat_file, &rs) == 0) {
                                    int s = rs.message_count - 50;
                                    if (s < 0) s = 0;
                                    for (int ri = s;
                                         ri < rs.message_count; ri++) {
                                        if (g_filter_handle[0] != '\0'
                                            && strcmp(
                                                rs.messages[ri].handle,
                                                g_filter_handle) != 0)
                                            continue;
                                        if (g_mention_handle[0] != '\0'
                                            && !content_mentions(
                                                rs.messages[ri].content,
                                                g_mention_handle))
                                            continue;
                                        format_message(
                                            rs.messages[ri].handle,
                                            rs.messages[ri].content,
                                            g_handle,
                                            rs.messages[ri].timestamp);
                                    }
                                    g_msg_count = rs.message_count;
                                    chat_state_free(&rs);
                                }
                            }
                            g_cursor_row = 0;
                            line_redraw(&edit, g_handle);
                        } else {
                            scrollback_render();
                        }
                    }
                    /* All other sequences ignored in scrollback */
                } else {
                    tcsetattr(STDIN_FILENO, TCSANOW, &tc_cur);
                }
            }
            /* All non-ESC input ignored in scrollback mode */
            continue;
        }

        /* Escape sequence handling */
        if (handle_escape_input(&edit, &esc, c, g_handle)) {
            continue;
        }

        /* Enter: submit immediately */
        if (c == '\n' || c == '\r') {
            int saved_cursor_row = g_cursor_row;
            printf("\n");
            g_cursor_row = 0;
            g_history_pos = -1;  /* Exit history browse mode */

            if (edit.len == 0) {
                /* Empty line: just reprint prompt, also poll */
                if (!poll_and_display(&edit, g_handle))
                    print_prompt(g_handle);
                continue;
            }

            /* Expand autocompletion: if the user typed a unique prefix
             * of a command (e.g. "/da") and hit Enter, expand to the
             * full command (e.g. "/dashboard") before dispatch. */
            {
                const char *comp = find_completion(edit.buf, edit.len);
                if (comp) {
                    size_t clen = strlen(comp);
                    line_ensure_cap(&edit, clen);
                    memcpy(edit.buf, comp, clen);
                    edit.buf[clen] = '\0';
                    edit.len = clen;
                    edit.cursor = clen;
                }
            }

            /* Record all non-empty input in history — messages and
             * commands alike — so up-arrow recalls /filter, /mention etc. */
            history_add(edit.buf);

            /* Erase typed command from display — slash commands are
             * not chat messages and should not clutter the screen. */
            if (edit.buf[0] == '/') {
                int up = saved_cursor_row + 1;
                if (up > 0)
                    printf("\033[%dA", up);
                printf("\r\033[J");
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
                if (!poll_and_display(&edit, g_handle))
                    print_prompt(g_handle);
                continue;
            }

            if (strcmp(edit.buf, "/paste") == 0) {
                line_state_reset(&edit);
                /* Enter alternate screen for paste mode */
                printf("\033[?1049h"); /* alternate screen */
                printf("\033[?25h");   /* show cursor */
                fflush(stdout);

                int submitted = paste_mode(&edit);

                /* Leave alternate screen */
                printf("\033[?1049l");
                printf("\033[?25h");
                printf("\033[0m");
                printf("\r\n");
                fflush(stdout);

                if (submitted && edit.len > 0) {
                    /* Strip trailing newlines from the message */
                    while (edit.len > 0 && edit.buf[edit.len - 1] == '\n') {
                        edit.len--;
                        edit.buf[edit.len] = '\0';
                    }
                    if (edit.len > 0) {
                        if (do_send(edit.buf) == 0) {
                            format_message(g_handle, edit.buf, g_handle, time(NULL));
                        } else {
                            printf("  %s(send failed)%s\n", DIM, RESET);
                        }
                    }
                } else {
                    printf("  %s(cancelled — not sent)%s\n", DIM, RESET);
                }
                line_state_reset(&edit);
                if (!poll_and_display(&edit, g_handle))
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

            /* /bash [command] — shell access with CWD memory */
            if (strcmp(edit.buf, "/bash") == 0 ||
                strncmp(edit.buf, "/bash ", 6) == 0) {

                const char *bash_cmd = NULL;
                if (strncmp(edit.buf, "/bash ", 6) == 0) {
                    bash_cmd = edit.buf + 6;
                    while (*bash_cmd == ' ') bash_cmd++;
                    if (*bash_cmd == '\0') bash_cmd = NULL;
                }

                if (!bash_cmd) {
                    /* Interactive mode: hand terminal to bash */
                    if (have_termios)
                        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);

                    /* Write cwd-on-exit trap to a temp file */
                    char trap_file[PATH_MAX];
                    snprintf(trap_file, sizeof(trap_file),
                             "/tmp/nbs-bash-%d.rc", (int)getpid());
                    char cwd_file[PATH_MAX];
                    snprintf(cwd_file, sizeof(cwd_file),
                             "/tmp/nbs-bash-%d.cwd", (int)getpid());
                    {
                        FILE *tf = fopen(trap_file, "w");
                        if (tf) {
                            fprintf(tf, "trap 'pwd > %s' EXIT\n",
                                    cwd_file);
                            if (g_bash_cwd[0])
                                fprintf(tf, "cd '%s' 2>/dev/null\n",
                                        g_bash_cwd);
                            fclose(tf);
                        }
                    }

                    pid_t bpid = fork();
                    if (bpid == 0) {
                        execlp("bash", "bash", "--rcfile", trap_file,
                               (char *)NULL);
                        _exit(127);
                    } else if (bpid > 0) {
                        int bst;
                        waitpid(bpid, &bst, 0);
                    }

                    /* Read back final cwd */
                    {
                        FILE *cf = fopen(cwd_file, "r");
                        if (cf) {
                            if (fgets(g_bash_cwd,
                                      sizeof(g_bash_cwd), cf)) {
                                size_t sl = strlen(g_bash_cwd);
                                if (sl > 0 &&
                                    g_bash_cwd[sl - 1] == '\n')
                                    g_bash_cwd[sl - 1] = '\0';
                            }
                            fclose(cf);
                            unlink(cwd_file);
                        }
                        unlink(trap_file);
                    }
                } else {
                    /* Captured mode: run command in a PTY,
                     * capture output, display in pager */
                    if (have_termios)
                        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);

                    /* Build the command with cwd prepended */
                    char full_cmd[8192];
                    if (g_bash_cwd[0])
                        snprintf(full_cmd, sizeof(full_cmd),
                                 "cd '%s' 2>/dev/null; %s",
                                 g_bash_cwd, bash_cmd);
                    else
                        snprintf(full_cmd, sizeof(full_cmd),
                                 "%s", bash_cmd);

                    /* Set up PTY with current terminal size */
                    struct winsize bws = {0};
                    ioctl(STDOUT_FILENO, TIOCGWINSZ, &bws);
                    if (bws.ws_row == 0) bws.ws_row = 24;
                    if (bws.ws_col == 0) bws.ws_col = 80;

                    int master_fd;
                    pid_t cpid = forkpty(&master_fd, NULL, NULL, &bws);
                    if (cpid == 0) {
                        /* Child: exec the command */
                        execlp("bash", "bash", "-c", full_cmd,
                               (char *)NULL);
                        _exit(127);
                    }

                    if (cpid < 0) {
                        printf("  %s(fork failed: %s)%s\n",
                               DIM, strerror(errno), RESET);
                        if (have_termios)
                            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
                        line_state_reset(&edit);
                        print_prompt(g_handle);
                        continue;
                    }

                    /* Parent: capture output, handle Ctrl-C */
                    printf("Running: %s%s%s  (Ctrl-C to cancel)\r\n",
                           DIM, bash_cmd, RESET);
                    fflush(stdout);

                    /* Switch stdin to raw for Ctrl-C detection */
                    struct termios cap_raw = orig_termios;
                    cap_raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
                    cap_raw.c_cc[VMIN] = 0;
                    cap_raw.c_cc[VTIME] = 1;
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &cap_raw);

                    #define BASH_MAX_LINES 65536
                    #define BASH_LINE_MAX  4096
                    char **blines = calloc(BASH_MAX_LINES,
                                           sizeof(char *));
                    int bline_count = 0;
                    char lbuf[BASH_LINE_MAX];
                    size_t lpos = 0;
                    int child_done = 0;

                    while (!child_done && blines) {
                        struct pollfd bpfds[2];
                        bpfds[0].fd = master_fd;
                        bpfds[0].events = POLLIN;
                        bpfds[1].fd = STDIN_FILENO;
                        bpfds[1].events = POLLIN;

                        int pr = poll(bpfds, 2, 200);
                        if (pr < 0 && errno == EINTR) continue;

                        /* Check for Ctrl-C on stdin */
                        if (bpfds[1].revents & POLLIN) {
                            char ic;
                            if (read(STDIN_FILENO, &ic, 1) == 1 &&
                                ic == 3) {
                                kill(cpid, SIGTERM);
                                usleep(100000);
                                kill(cpid, SIGKILL);
                                break;
                            }
                        }

                        /* Read output from PTY */
                        if (bpfds[0].revents & POLLIN) {
                            char rbuf[4096];
                            ssize_t rn = read(master_fd,
                                              rbuf, sizeof(rbuf));
                            if (rn <= 0) {
                                child_done = 1;
                            } else {
                                for (ssize_t ri = 0; ri < rn; ri++) {
                                    if (rbuf[ri] == '\n' ||
                                        rbuf[ri] == '\r') {
                                        if (lpos > 0 &&
                                            bline_count <
                                                BASH_MAX_LINES) {
                                            lbuf[lpos] = '\0';
                                            blines[bline_count] =
                                                strdup(lbuf);
                                            bline_count++;
                                            lpos = 0;
                                        }
                                    } else if (lpos <
                                               BASH_LINE_MAX - 1) {
                                        lbuf[lpos++] = rbuf[ri];
                                    }
                                }
                            }
                        }
                        if (bpfds[0].revents & (POLLHUP | POLLERR))
                            child_done = 1;

                        /* Check if child exited */
                        {
                            int cst;
                            pid_t wr = waitpid(cpid, &cst, WNOHANG);
                            if (wr > 0) child_done = 1;
                        }
                    }

                    /* Flush remaining partial line */
                    if (lpos > 0 && blines &&
                        bline_count < BASH_MAX_LINES) {
                        lbuf[lpos] = '\0';
                        blines[bline_count] = strdup(lbuf);
                        bline_count++;
                    }

                    /* Drain any remaining output */
                    if (blines) {
                        char rbuf[4096];
                        ssize_t rn;
                        while ((rn = read(master_fd,
                                          rbuf, sizeof(rbuf))) > 0) {
                            for (ssize_t ri = 0; ri < rn; ri++) {
                                if (rbuf[ri] == '\n' ||
                                    rbuf[ri] == '\r') {
                                    if (lpos > 0 &&
                                        bline_count < BASH_MAX_LINES) {
                                        lbuf[lpos] = '\0';
                                        blines[bline_count] =
                                            strdup(lbuf);
                                        bline_count++;
                                        lpos = 0;
                                    }
                                } else if (lpos < BASH_LINE_MAX - 1) {
                                    lbuf[lpos++] = rbuf[ri];
                                }
                            }
                        }
                        if (lpos > 0 &&
                            bline_count < BASH_MAX_LINES) {
                            lbuf[lpos] = '\0';
                            blines[bline_count] = strdup(lbuf);
                            bline_count++;
                        }
                    }

                    close(master_fd);
                    waitpid(cpid, NULL, 0);

                    /* Display captured output in pager */
                    if (blines && bline_count > 0) {
                        /* Enter raw mode + alternate screen */
                        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
                        printf("\033[?1049h\033[?25l");
                        fflush(stdout);

                        int bscroll = 0;
                        int bdirty = 1;

                        while (1) {
                            if (bdirty) {
                                struct winsize pws;
                                int prows = 24, pcols = 80;
                                if (ioctl(STDOUT_FILENO, TIOCGWINSZ,
                                          &pws) == 0) {
                                    prows = pws.ws_row;
                                    pcols = pws.ws_col;
                                }
                                int pcontent = prows - 2;

                                printf("\033[H\033[2J");
                                /* Header */
                                printf("\033[7m BASH \033[0m "
                                       "\033[2m%s\033[0m", bash_cmd);
                                char bpos[48];
                                snprintf(bpos, sizeof(bpos),
                                         "%d-%d / %d",
                                         bscroll + 1,
                                         bscroll + pcontent <
                                             bline_count ?
                                             bscroll + pcontent :
                                             bline_count,
                                         bline_count);
                                int bhdr = 6 + 1 +
                                           (int)strlen(bash_cmd);
                                int bpad = pcols - bhdr -
                                           (int)strlen(bpos);
                                if (bpad > 0)
                                    printf("%*s", bpad, "");
                                printf("\033[2m%s\033[0m\r\n", bpos);

                                /* Content */
                                for (int bi = 0; bi < pcontent; bi++) {
                                    int bli = bscroll + bi;
                                    if (bli < bline_count)
                                        printf("%s\033[0m\r\n",
                                               blines[bli]);
                                    else
                                        printf("\033[2m~\033[0m\r\n");
                                }

                                /* Hint */
                                printf("\033[%d;1H\033[2K\033[2m"
                                       "Arrows/PgUp/PgDn: scroll  "
                                       "ESC/q: close\033[0m", prows);
                                fflush(stdout);
                                bdirty = 0;
                            }

                            /* Read key (non-blocking with VTIME) */
                            char kc;
                            if (read(STDIN_FILENO, &kc, 1) != 1)
                                continue;

                            struct winsize pws2;
                            int pcontent2 = 22;
                            if (ioctl(STDOUT_FILENO, TIOCGWINSZ,
                                      &pws2) == 0)
                                pcontent2 = pws2.ws_row - 2;

                            if (kc == 27) { /* ESC or arrow */
                                char seq[3];
                                struct termios tc, tt;
                                tcgetattr(STDIN_FILENO, &tc);
                                tt = tc;
                                tt.c_cc[VMIN] = 0;
                                tt.c_cc[VTIME] = 1;
                                tcsetattr(STDIN_FILENO, TCSANOW, &tt);
                                ssize_t snr = read(STDIN_FILENO,
                                                   &seq[0], 1);
                                if (snr <= 0) {
                                    tcsetattr(STDIN_FILENO, TCSANOW,
                                              &tc);
                                    break; /* bare ESC — exit */
                                }
                                if (seq[0] == '[') {
                                    read(STDIN_FILENO, &seq[1], 1);
                                    tcsetattr(STDIN_FILENO, TCSANOW,
                                              &tc);
                                    if (seq[1] == 'A') { /* Up */
                                        if (bscroll > 0)
                                            { bscroll--; bdirty = 1; }
                                    } else if (seq[1] == 'B') {
                                        if (bscroll <
                                            bline_count - pcontent2)
                                            { bscroll++; bdirty = 1; }
                                    } else if (seq[1] == '5') {
                                        char t;
                                        read(STDIN_FILENO, &t, 1);
                                        bscroll -= pcontent2;
                                        if (bscroll < 0) bscroll = 0;
                                        bdirty = 1;
                                    } else if (seq[1] == '6') {
                                        char t;
                                        read(STDIN_FILENO, &t, 1);
                                        bscroll += pcontent2;
                                        if (bscroll >
                                            bline_count - pcontent2)
                                            bscroll = bline_count -
                                                      pcontent2;
                                        if (bscroll < 0) bscroll = 0;
                                        bdirty = 1;
                                    }
                                } else {
                                    tcsetattr(STDIN_FILENO, TCSANOW,
                                              &tc);
                                }
                                continue;
                            }
                            if (kc == 'q') break;
                            if (kc == 'j' || kc == 'k') {
                                if (kc == 'k' && bscroll > 0)
                                    { bscroll--; bdirty = 1; }
                                if (kc == 'j' &&
                                    bscroll < bline_count - pcontent2)
                                    { bscroll++; bdirty = 1; }
                            }
                            if (kc == ' ') {
                                bscroll += pcontent2;
                                if (bscroll > bline_count - pcontent2)
                                    bscroll = bline_count - pcontent2;
                                if (bscroll < 0) bscroll = 0;
                                bdirty = 1;
                            }
                        }

                        /* Leave pager */
                        printf("\033[?25h\033[?1049l");
                        fflush(stdout);
                    } else {
                        /* No output — just restore terminal */
                        if (have_termios)
                            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
                        printf("  %s(no output)%s\n", DIM, RESET);
                    }

                    /* Capture cwd: run pwd in the same cwd */
                    {
                        char pwd_cmd[8192];
                        if (g_bash_cwd[0])
                            snprintf(pwd_cmd, sizeof(pwd_cmd),
                                     "cd '%s' 2>/dev/null && pwd",
                                     g_bash_cwd);
                        else
                            snprintf(pwd_cmd, sizeof(pwd_cmd), "pwd");
                        FILE *pp = popen(pwd_cmd, "r");
                        if (pp) {
                            if (fgets(g_bash_cwd,
                                      sizeof(g_bash_cwd), pp)) {
                                size_t sl = strlen(g_bash_cwd);
                                if (sl > 0 &&
                                    g_bash_cwd[sl - 1] == '\n')
                                    g_bash_cwd[sl - 1] = '\0';
                            }
                            pclose(pp);
                        }
                    }

                    if (blines) {
                        for (int bi = 0; bi < bline_count; bi++)
                            free(blines[bi]);
                        free(blines);
                    }
                }

                /* Restore terminal state and redraw chat */
                printf("\033[?1049l\033[?25h\033[0m\r\n");
                fflush(stdout);
                if (have_termios)
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

                printf("\033[2J\033[H");
                {
                    chat_state_t redraw_state;
                    if (chat_read(g_chat_file, &redraw_state) == 0) {
                        int start = redraw_state.message_count - 50;
                        if (start < 0) start = 0;
                        for (int i = start;
                             i < redraw_state.message_count; i++) {
                            if (g_filter_handle[0] != '\0' &&
                                strcmp(redraw_state.messages[i].handle,
                                       g_filter_handle) != 0)
                                continue;
                            if (g_mention_handle[0] != '\0' &&
                                !content_mentions(
                                    redraw_state.messages[i].content,
                                    g_mention_handle))
                                continue;
                            format_message(
                                redraw_state.messages[i].handle,
                                redraw_state.messages[i].content,
                                g_handle,
                                redraw_state.messages[i].timestamp);
                        }
                        g_msg_count = redraw_state.message_count;
                        chat_state_free(&redraw_state);
                    }
                }

                line_state_reset(&edit);
                g_cursor_row = 0;
                print_prompt(g_handle);
                continue;
            }

            /* /browse [pattern] — scrollable full-screen chat view */
            if (strcmp(edit.buf, "/browse") == 0 ||
                strncmp(edit.buf, "/browse ", 8) == 0) {
                const char *pattern = NULL;
                if (strncmp(edit.buf, "/browse ", 8) == 0) {
                    pattern = edit.buf + 8;
                    while (*pattern == ' ') pattern++;
                    if (*pattern == '\0') pattern = NULL;
                }

                chat_state_t browse_state;
                if (chat_read(g_chat_file, &browse_state) != 0) {
                    printf("  %s(browse failed — could not read chat)%s\n",
                           DIM, RESET);
                    line_state_reset(&edit);
                    print_prompt(g_handle);
                    continue;
                }

                /* Save terminal state and enter browse mode */
                if (have_termios) {
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
                }

                chatview_t *browse = chatview_init(&browse_state, g_chat_file);
                if (browse) {
                    if (pattern)
                        chatview_search(browse, pattern);
                    /* Poll for new messages during browse */
                    chatview_set_poll(browse, browse_poll_cb,
                                     (void *)g_chat_file);
                    chatview_run(browse);
                    chatview_free(browse);
                } else {
                    chat_state_free(&browse_state);
                    printf("  %s(browse failed — out of memory)%s\n",
                           DIM, RESET);
                }

                /* Restore terminal state */
                printf("\033[?1049l");  /* leave alternate screen */
                printf("\033[?25h");    /* show cursor */
                printf("\033[0m");      /* reset attributes */
                printf("\r\n");
                fflush(stdout);
                if (have_termios) {
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
                }

                /* Redraw — clear screen and repaint last 50 messages
                 * so the user returns to a clean chat view, not stale
                 * scrollback with the /browse command visible. */
                printf("\033[2J\033[H");
                {
                    chat_state_t redraw_state;
                    if (chat_read(g_chat_file, &redraw_state) == 0) {
                        int start = redraw_state.message_count - 50;
                        if (start < 0) start = 0;
                        for (int i = start; i < redraw_state.message_count; i++) {
                            if (g_filter_handle[0] != '\0' &&
                                strcmp(redraw_state.messages[i].handle,
                                       g_filter_handle) != 0)
                                continue;
                            if (g_mention_handle[0] != '\0' &&
                                !content_mentions(redraw_state.messages[i].content,
                                                  g_mention_handle))
                                continue;
                            format_message(redraw_state.messages[i].handle,
                                          redraw_state.messages[i].content,
                                          g_handle,
                                          redraw_state.messages[i].timestamp);
                        }
                        g_msg_count = redraw_state.message_count;
                        chat_state_free(&redraw_state);
                    }
                }

                line_state_reset(&edit);
                g_cursor_row = 0;
                print_prompt(g_handle);
                continue;
            }

            /* /dashboard — live full-screen team dashboard */
            if (strcmp(edit.buf, "/dashboard") == 0) {
                if (g_watchdog.project_root[0] == '\0') {
                    printf("  %s(dashboard requires a project root — "
                           "start with --restart or --goal-file)%s\n",
                           DIM, RESET);
                    line_state_reset(&edit);
                    print_prompt(g_handle);
                    continue;
                }

                /* Save terminal state */
                if (have_termios) {
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
                }

                dashboard_t *dash = dashboard_init(
                    g_watchdog.project_root);
                if (dash) {
                    dashboard_run(dash);
                    dashboard_free(dash);
                } else {
                    printf("  %s(dashboard failed — could not "
                           "initialise)%s\n", DIM, RESET);
                }

                /* Restore terminal state */
                printf("\033[?1049l");
                printf("\033[?25h");
                printf("\033[0m");
                printf("\r\n");
                fflush(stdout);
                if (have_termios) {
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
                }

                /* Redraw chat */
                printf("\033[2J\033[H");
                {
                    chat_state_t redraw_state;
                    if (chat_read(g_chat_file, &redraw_state) == 0) {
                        int start = redraw_state.message_count - 50;
                        if (start < 0) start = 0;
                        for (int i = start;
                             i < redraw_state.message_count; i++) {
                            if (g_filter_handle[0] != '\0' &&
                                strcmp(redraw_state.messages[i].handle,
                                       g_filter_handle) != 0)
                                continue;
                            if (g_mention_handle[0] != '\0' &&
                                !content_mentions(
                                    redraw_state.messages[i].content,
                                    g_mention_handle))
                                continue;
                            format_message(
                                redraw_state.messages[i].handle,
                                redraw_state.messages[i].content,
                                g_handle,
                                redraw_state.messages[i].timestamp);
                        }
                        g_msg_count = redraw_state.message_count;
                        chat_state_free(&redraw_state);
                    }
                }

                line_state_reset(&edit);
                g_cursor_row = 0;
                print_prompt(g_handle);
                continue;
            }

            /* /file [path] — full-screen file browser with directory memory */
            if (strcmp(edit.buf, "/file") == 0 ||
                strncmp(edit.buf, "/file ", 6) == 0) {

                /* Determine start path: argument > last dir > cwd */
                const char *start_path = NULL;
                if (strncmp(edit.buf, "/file ", 6) == 0) {
                    start_path = edit.buf + 6;
                    while (*start_path == ' ') start_path++;
                    if (*start_path == '\0') start_path = NULL;
                }
                if (!start_path && g_file_last_dir[0] != '\0')
                    start_path = g_file_last_dir;

                /* State file for directory memory */
                char state_path[PATH_MAX];
                snprintf(state_path, sizeof(state_path),
                         "/tmp/nbs-fb-%d.state", (int)getpid());

                /* Save terminal state */
                if (have_termios) {
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
                }

                /* Fork/exec nbs-file-browser */
                pid_t pid = fork();
                if (pid == 0) {
                    char sf_arg[PATH_MAX + 16];
                    snprintf(sf_arg, sizeof(sf_arg),
                             "--state-file=%s", state_path);
                    if (start_path)
                        execlp("nbs-file-browser", "nbs-file-browser",
                               sf_arg, start_path, (char *)NULL);
                    else
                        execlp("nbs-file-browser", "nbs-file-browser",
                               sf_arg, (char *)NULL);
                    _exit(127);
                } else if (pid > 0) {
                    int wst;
                    waitpid(pid, &wst, 0);
                }

                /* Read back last directory from state file */
                {
                    FILE *sf = fopen(state_path, "r");
                    if (sf) {
                        if (fgets(g_file_last_dir,
                                  sizeof(g_file_last_dir), sf)) {
                            /* Strip trailing newline */
                            size_t slen = strlen(g_file_last_dir);
                            if (slen > 0 &&
                                g_file_last_dir[slen - 1] == '\n')
                                g_file_last_dir[slen - 1] = '\0';
                        }
                        fclose(sf);
                        unlink(state_path);
                    }
                }

                /* Restore terminal state */
                printf("\033[?1049l");
                printf("\033[?25h");
                printf("\033[0m");
                printf("\r\n");
                fflush(stdout);
                if (have_termios) {
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
                }

                /* Redraw chat */
                printf("\033[2J\033[H");
                {
                    chat_state_t redraw_state;
                    if (chat_read(g_chat_file, &redraw_state) == 0) {
                        int start = redraw_state.message_count - 50;
                        if (start < 0) start = 0;
                        for (int i = start;
                             i < redraw_state.message_count; i++) {
                            if (g_filter_handle[0] != '\0' &&
                                strcmp(redraw_state.messages[i].handle,
                                       g_filter_handle) != 0)
                                continue;
                            if (g_mention_handle[0] != '\0' &&
                                !content_mentions(
                                    redraw_state.messages[i].content,
                                    g_mention_handle))
                                continue;
                            format_message(
                                redraw_state.messages[i].handle,
                                redraw_state.messages[i].content,
                                g_handle,
                                redraw_state.messages[i].timestamp);
                        }
                        g_msg_count = redraw_state.message_count;
                        chat_state_free(&redraw_state);
                    }
                }

                line_state_reset(&edit);
                g_cursor_row = 0;
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

            /* /mention <handle> — show only messages that @mention a handle */
            if (strncmp(edit.buf, "/mention ", 9) == 0) {
                const char *target = edit.buf + 9;
                while (*target == ' ') target++;
                if (*target == '\0') {
                    printf("  %sUsage: /mention <handle>%s\n", DIM, RESET);
                } else {
                    snprintf(g_mention_handle, sizeof(g_mention_handle), "%s", target);
                    printf("  %sMention filter: showing messages mentioning @%s%s\n",
                           DIM, g_mention_handle, RESET);
                    chat_state_t mstate;
                    if (chat_read(g_chat_file, &mstate) == 0) {
                        int matches[50];
                        int match_count = 0;
                        for (int i = mstate.message_count - 1; i >= 0 && match_count < 50; i--) {
                            if (content_mentions(mstate.messages[i].content, g_mention_handle)) {
                                matches[match_count++] = i;
                            }
                        }
                        for (int j = match_count - 1; j >= 0; j--) {
                            int i = matches[j];
                            format_message(mstate.messages[i].handle,
                                          mstate.messages[i].content, g_handle,
                                          mstate.messages[i].timestamp);
                        }
                        if (match_count == 0) {
                            printf("  %sNo messages mentioning @%s%s\n",
                                   DIM, g_mention_handle, RESET);
                        }
                        chat_state_free(&mstate);
                    }
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            if (strcmp(edit.buf, "/mention") == 0) {
                if (g_mention_handle[0] != '\0') {
                    printf("  %sMention filter active: @%s%s\n",
                           DIM, g_mention_handle, RESET);
                } else {
                    printf("  %sNo mention filter. Usage: /mention <handle>%s\n",
                           DIM, RESET);
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* /unmention — clear mention filter */
            if (strcmp(edit.buf, "/unmention") == 0) {
                if (g_mention_handle[0] != '\0') {
                    g_mention_handle[0] = '\0';
                    printf("  %sMention filter cleared — showing last 20 messages%s\n",
                           DIM, RESET);
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
                    printf("  %sNo mention filter active%s\n", DIM, RESET);
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* /redraw — clear screen, repaint last 50 messages */
            if (strcmp(edit.buf, "/redraw") == 0) {
                printf("\033[2J\033[H");
                chat_state_t redraw_state;
                if (chat_read(g_chat_file, &redraw_state) == 0) {
                    int start = redraw_state.message_count - 50;
                    if (start < 0) start = 0;
                    for (int i = start; i < redraw_state.message_count; i++) {
                        if (g_filter_handle[0] != '\0' &&
                            strcmp(redraw_state.messages[i].handle,
                                   g_filter_handle) != 0)
                            continue;
                        if (g_mention_handle[0] != '\0' &&
                            !content_mentions(redraw_state.messages[i].content,
                                              g_mention_handle))
                            continue;
                        format_message(redraw_state.messages[i].handle,
                                      redraw_state.messages[i].content,
                                      g_handle,
                                      redraw_state.messages[i].timestamp);
                    }
                    g_msg_count = redraw_state.message_count;
                    chat_state_free(&redraw_state);
                }
                g_cursor_row = 0;
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* /shutdown — announce, wait 10s, kill all agents */
            if (strcmp(edit.buf, "/shutdown") == 0) {
                line_state_reset(&edit);
                if (g_watchdog.project_root[0] == '\0') {
                    info_line_emit(&edit, g_handle, "shutdown",
                                   "No project root — nothing to shut down.");
                    continue;
                }
                do_send("@team SYSTEM: Shutting down in 10 seconds. "
                        "Finish your current action and save state.");
                watchdog_disable(&g_watchdog);
                info_line_emit(&edit, g_handle, "shutdown",
                               "Shutting down in 10 seconds...");
                sleep(10);

                /* Kill all sessions for this project — run twice with
                 * a gap to catch oracles spawned during the 10s grace.
                 * Uses absolute path to nbs-ts to avoid PATH issues. */
                {
                    char tag[4096];
                    const char *chat_base = strrchr(g_watchdog.chat_path, '/');
                    if (chat_base) chat_base++; else chat_base = g_watchdog.chat_path;
                    snprintf(tag, sizeof(tag), "%s", chat_base);
                    /* Strip .chat suffix */
                    char *dot = strstr(tag, ".chat");
                    if (dot) *dot = '\0';
                    /* Replace dots with dashes */
                    for (char *p = tag; *p; p++) if (*p == '.') *p = '-';

                    /* Kill sessions using fork+exec — avoids shell
                     * command string truncation issues. Run twice with
                     * a gap to catch oracles spawned during the grace. */
                    for (int sweep = 0; sweep < 2; sweep++) {
                        if (sweep > 0) sleep(2);
                        /* Use exec_fire_and_forget pattern: fork, exec
                         * a small shell snippet with the tag. */
                        pid_t kpid2 = fork();
                        if (kpid2 == 0) {
                            execlp("sh", "sh", "-c",
                                   "nbs-ts list --name=\"$1\" 2>/dev/null | "
                                   "cut -f1 | while read h; do "
                                   "nbs-ts kill \"$h\" 2>/dev/null; done",
                                   "sh", tag, (char *)NULL);
                            _exit(127);
                        }
                        if (kpid2 > 0) {
                            int wst;
                            waitpid(kpid2, &wst, 0);
                        }
                    }
                }
                /* Kill sidecars and nbs-claude for this project */
                {
                    char pat[4200];
                    pid_t kpid;

                    snprintf(pat, sizeof(pat),
                             "nbs-sidecar.*--root=%s", g_watchdog.project_root);
                    kpid = fork();
                    if (kpid == 0) {
                        execlp("pkill", "pkill", "-9", "-f", pat, (char *)NULL);
                        _exit(0);
                    }
                    if (kpid > 0) waitpid(kpid, NULL, 0);

                    snprintf(pat, sizeof(pat),
                             "nbs-claude.*%s.*dangerously", g_watchdog.project_root);
                    kpid = fork();
                    if (kpid == 0) {
                        execlp("pkill", "pkill", "-9", "-f", pat, (char *)NULL);
                        _exit(0);
                    }
                    if (kpid > 0) waitpid(kpid, NULL, 0);
                }
                info_line_emit(&edit, g_handle, "shutdown", "Team stopped.");
                continue;
            }

            /* /pause — freeze team in place, no process kills */
            if (strcmp(edit.buf, "/pause") == 0) {
                if (g_watchdog.project_root[0] == '\0') {
                    printf("  %sWatchdog not initialised — cannot pause.%s\n",
                           DIM, RESET);
                    line_state_reset(&edit);
                    print_prompt(g_handle);
                    continue;
                }
                char pause_path[8192];
                snprintf(pause_path, sizeof(pause_path),
                         "%s/.nbs/control-pause", g_watchdog.project_root);
                struct stat pst;
                if (stat(pause_path, &pst) == 0) {
                    printf("  %sTeam already paused.%s\n", DIM, RESET);
                } else {
                    /* Create pause file with timestamp */
                    FILE *pf = fopen(pause_path, "w");
                    if (pf) {
                        fprintf(pf, "%lld\n", (long long)time(NULL));
                        fclose(pf);
                    }
                    watchdog_disable(&g_watchdog);
                    do_send("@team SYSTEM: Team paused. Stop all work immediately. "
                            "Do not start any new tasks or tool calls. "
                            "This is a temporary pause — you will be resumed shortly.");
                    printf("  %sTeam paused. Type /resume to continue.%s\n",
                           DIM, RESET);
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* /resume — wake up paused team */
            if (strcmp(edit.buf, "/resume") == 0) {
                if (g_watchdog.project_root[0] == '\0') {
                    printf("  %sWatchdog not initialised — cannot resume.%s\n",
                           DIM, RESET);
                    line_state_reset(&edit);
                    print_prompt(g_handle);
                    continue;
                }
                char pause_path[8192];
                snprintf(pause_path, sizeof(pause_path),
                         "%s/.nbs/control-pause", g_watchdog.project_root);
                struct stat pst;
                int had_pause_file = (stat(pause_path, &pst) == 0);
                if (had_pause_file)
                    unlink(pause_path);

                /* Always re-enable the watchdog. /shutdown disables it
                 * without creating a pause file, so checking only the
                 * file would leave the watchdog permanently disabled. */
                if (!watchdog_is_enabled(&g_watchdog) || had_pause_file) {
                    watchdog_enable(&g_watchdog);

                    /* Reset all agent cursors to msg_count-1 so they
                     * start fresh from the resume message, not from
                     * wherever they were when paused. The pause-era
                     * backlog is monitoring noise — agents don't need
                     * to process it. */
                    {
                        chat_state_t rs;
                        if (chat_read(g_chat_file, &rs) == 0) {
                            int mc = rs.message_count > 0
                                     ? rs.message_count - 1 : 0;
                            const char *agents[] = {
                                "supervisor", "generalist", "gatekeeper",
                                "theologian", "testkeeper", "scribe", "medic"
                            };
                            /* Find nbs-chat for cursor-set */
                            char nbs_chat_bin[4096];
                            int found = 0;
                            int sn = snprintf(nbs_chat_bin, sizeof(nbs_chat_bin),
                                              "%s/.nbs/bin/nbs-chat",
                                              g_watchdog.project_root);
                            if (sn > 0 && (size_t)sn < sizeof(nbs_chat_bin) &&
                                access(nbs_chat_bin, X_OK) == 0)
                                found = 1;
                            if (!found) {
                                sn = snprintf(nbs_chat_bin, sizeof(nbs_chat_bin),
                                              "%s/bin/nbs-chat",
                                              g_watchdog.project_root);
                                if (sn > 0 && (size_t)sn < sizeof(nbs_chat_bin) &&
                                    access(nbs_chat_bin, X_OK) == 0)
                                    found = 1;
                            }
                            if (found) {
                                char mc_str[32];
                                snprintf(mc_str, sizeof(mc_str), "%d", mc);
                                for (int a = 0; a < 7; a++) {
                                    /* fork+exec nbs-chat cursor-set */
                                    pid_t cp = fork();
                                    if (cp == 0) {
                                        execlp(nbs_chat_bin, "nbs-chat",
                                               "cursor-set", g_chat_file,
                                               agents[a], mc_str,
                                               (char *)NULL);
                                        _exit(127);
                                    } else if (cp > 0) {
                                        int ws;
                                        waitpid(cp, &ws, 0);
                                    }
                                }
                            }
                            chat_state_free(&rs);
                        }
                    }

                    do_send("@team SYSTEM: Team resumed. Continue where you left off.");
                    printf("  %sTeam resumed.%s\n", DIM, RESET);
                } else {
                    printf("  %sTeam is not paused.%s\n", DIM, RESET);
                }
                line_state_reset(&edit);
                print_prompt(g_handle);
                continue;
            }

            /* /restart — manual team restart (bypasses rate limit) */
            if (strcmp(edit.buf, "/restart") == 0) {
                line_state_reset(&edit);
                if (!watchdog_is_enabled(&g_watchdog)) {
                    info_line_emit(&edit, g_handle, "restart",
                                   "Watchdog not initialised — cannot restart.");
                } else {
                    info_line_emit(&edit, g_handle, "restart",
                                   "Triggering manual restart...");
                    char rscript[4096 + 64];
                    if (resolve_restart_script(g_watchdog.project_root,
                                               rscript, sizeof(rscript)) == 0) {
                        const char *restart_argv[] = {
                            "bash", rscript,
                            g_watchdog.project_root,
                            g_watchdog.chat_path, NULL
                        };
                        spawn_with_capture("restart", restart_argv);
                    }
                }
                continue;
            }

            /* /kick <agent> — hard restart a single agent */
            if (strncmp(edit.buf, "/kick ", 6) == 0 ||
                strcmp(edit.buf, "/kick") == 0) {
                const char *agent = (edit.len > 5) ? edit.buf + 6 : NULL;
                line_state_reset(&edit);

                if (g_watchdog.project_root[0] == '\0') {
                    info_line_emit(&edit, g_handle, "kick",
                                   "No project root — cannot kick.");
                } else if (!agent || agent[0] == '\0') {
                    info_line_emit(&edit, g_handle, "kick",
                                   "Usage: /kick <agent> (e.g. /kick scribe)");
                } else {
                    /* Find nbs-kick-agent: .nbs/bin/, bin/, or PATH */
                    const char *kick_bin = "nbs-kick-agent";
                    char kick_path[4096];
                    int kn = snprintf(kick_path, sizeof(kick_path),
                                      "%s/.nbs/bin/nbs-kick-agent",
                                      g_watchdog.project_root);
                    if (kn > 0 && (size_t)kn < sizeof(kick_path) &&
                        access(kick_path, X_OK) == 0) {
                        kick_bin = kick_path;
                    } else {
                        kn = snprintf(kick_path, sizeof(kick_path),
                                      "%s/bin/nbs-kick-agent",
                                      g_watchdog.project_root);
                        if (kn > 0 && (size_t)kn < sizeof(kick_path) &&
                            access(kick_path, X_OK) == 0) {
                            kick_bin = kick_path;
                        }
                        /* else: fall through to "nbs-kick-agent" from PATH */
                    }
                    {
                        char agent_arg[64];
                        snprintf(agent_arg, sizeof(agent_arg), "%s", agent);
                        const char *kick_argv[] = {
                            "bash", kick_bin, agent_arg,
                            g_watchdog.project_root,
                            g_watchdog.chat_path, NULL
                        };
                        spawn_with_capture("kick", kick_argv);
                    }
                }
                continue;
            }

            /* /health — report team health */
            if (strcmp(edit.buf, "/health") == 0) {
                line_state_reset(&edit);
                if (g_watchdog.project_root[0] == '\0') {
                    info_line_emit(&edit, g_handle, "health",
                                   "No project root — cannot check health.");
                } else {
                    /* Derive chat tag for nbs-team-check */
                    char health_tag[256];
                    {
                        const char *base = strrchr(g_watchdog.chat_path, '/');
                        base = base ? base + 1 : g_watchdog.chat_path;
                        size_t blen = strlen(base);
                        if (blen > 5 && strcmp(base + blen - 5, ".chat") == 0)
                            blen -= 5;
                        if (blen >= sizeof(health_tag)) blen = sizeof(health_tag) - 1;
                        memcpy(health_tag, base, blen);
                        health_tag[blen] = '\0';
                        for (size_t i = 0; i < blen; i++)
                            if (health_tag[i] == '.') health_tag[i] = '-';
                    }
                    const char *health_argv[] = {
                        "nbs-team-check", health_tag,
                        g_watchdog.project_root, NULL
                    };
                    spawn_with_capture("health", health_argv);
                }
                continue;
            }

            /* /sidecar [handle] — restart sidecars */
            if (strcmp(edit.buf, "/sidecar") == 0 ||
                strncmp(edit.buf, "/sidecar ", 9) == 0) {
                /* Extract optional handle argument */
                const char *sc_handle = NULL;
                if (strncmp(edit.buf, "/sidecar ", 9) == 0) {
                    sc_handle = edit.buf + 9;
                    while (*sc_handle == ' ') sc_handle++;
                    if (*sc_handle == '\0') sc_handle = NULL;
                }
                line_state_reset(&edit);
                if (g_watchdog.project_root[0] == '\0') {
                    info_line_emit(&edit, g_handle, "sidecar",
                                   "No project root — cannot restart sidecars.");
                } else {
                    if (sc_handle) {
                        char msg[128];
                        snprintf(msg, sizeof(msg),
                                 "Restarting sidecar for %s...", sc_handle);
                        info_line_emit(&edit, g_handle, "sidecar", msg);
                    } else {
                        info_line_emit(&edit, g_handle, "sidecar",
                                       "Restarting sidecars (respawning missing)...");
                    }
                    /* Find nbs-sidecar-restart: .nbs/bin/ then bin/ */
                    char sc_path[4096];
                    int sn = snprintf(sc_path, sizeof(sc_path),
                                      "%s/.nbs/bin/nbs-sidecar-restart",
                                      g_watchdog.project_root);
                    if (sn <= 0 || (size_t)sn >= sizeof(sc_path) ||
                        access(sc_path, X_OK) != 0) {
                        sn = snprintf(sc_path, sizeof(sc_path),
                                      "%s/bin/nbs-sidecar-restart",
                                      g_watchdog.project_root);
                    }
                    if (sn > 0 && (size_t)sn < sizeof(sc_path) &&
                        access(sc_path, X_OK) == 0) {
                        char root_flag[4200];
                        snprintf(root_flag, sizeof(root_flag),
                                 "--root=%s", g_watchdog.project_root);
                        if (sc_handle) {
                            const char *sc_argv[] = {
                                sc_path, root_flag, sc_handle, NULL
                            };
                            spawn_with_capture("sidecar", sc_argv);
                        } else {
                            const char *sc_argv[] = {
                                sc_path, "--respawn", root_flag, NULL
                            };
                            spawn_with_capture("sidecar", sc_argv);
                        }
                    } else {
                        info_line_emit(&edit, g_handle, "sidecar",
                                       "nbs-sidecar-restart not found.");
                    }
                }
                continue;
            }

            /* Trigger commands: /pythia, /shepard, /librarian, /fixup */
            if (strcmp(edit.buf, "/pythia") == 0 ||
                strcmp(edit.buf, "/shepard") == 0 ||
                strcmp(edit.buf, "/librarian") == 0 ||
                strcmp(edit.buf, "/fixup") == 0 ||
                strcmp(edit.buf, "/digest") == 0) {
                /* Save role before resetting — role pointed into edit.buf */
                char role_buf[16];
                snprintf(role_buf, sizeof(role_buf), "%s", edit.buf + 1);
                const char *role = role_buf;
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
                } else if (strcmp(role, "digest") == 0) {
                    role = TRIGGER_ROLE_DIGEST;
                    desc = TRIGGER_DESC_DIGEST;
                    skill = TRIGGER_SKILL_DIGEST;
                }
                line_state_reset(&edit);

                /* Oracles work while paused — they read chat and post
                 * results, they don't need active sidecars. This lets
                 * the human /pause, /digest, /pythia, /resume. */
                if (g_watchdog.project_root[0] == '\0') {
                    info_line_emit(&edit, g_handle, role,
                                   "No project root — watchdog not initialised.");
                } else if (!desc || !skill) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Unknown oracle: %s", role);
                    info_line_emit(&edit, g_handle, role, msg);
                } else {
                    int sr = spawn_trigger_worker(role, skill, desc,
                                                  g_watchdog.project_root);
                    char msg[160];
                    if (sr == 0) {
                        snprintf(msg, sizeof(msg),
                                 "%s spawned (will post to chat when done).",
                                 role);
                    } else if (sr == 1) {
                        snprintf(msg, sizeof(msg),
                                 "%s already running — skipped (no duplicate spawn).",
                                 role);
                    } else {
                        snprintf(msg, sizeof(msg),
                                 "Failed to spawn %s.", role);
                    }
                    info_line_emit(&edit, g_handle, role, msg);
                }
                /* info_line_emit already redraws the prompt via line_redraw */
                continue;
            }

            /* /broadcast <msg> — send text directly to all agent terminals. */
            if (strncmp(edit.buf, "/broadcast ", 11) == 0 ||
                strcmp(edit.buf, "/broadcast") == 0) {
                const char *bmsg = edit.buf + 10;
                while (*bmsg == ' ') bmsg++;
                if (*bmsg == '\0') {
                    line_state_reset(&edit);
                    info_line_emit(&edit, g_handle, "broadcast",
                                   "Usage: /broadcast <message>");
                    continue;
                }

                /* Derive tag from chat file basename */
                char btag[256] = {0};
                {
                    const char *base = strrchr(g_chat_file, '/');
                    base = base ? base + 1 : g_chat_file;
                    size_t blen = strlen(base);
                    if (blen > 5 && strcmp(base + blen - 5, ".chat") == 0)
                        blen -= 5;
                    if (blen >= sizeof(btag)) blen = sizeof(btag) - 1;
                    memcpy(btag, base, blen);
                    btag[blen] = '\0';
                    for (size_t i = 0; i < blen; i++)
                        if (btag[i] == '.') btag[i] = '-';
                }

                /* Get alive sessions for this project via nbs-ts list */
                int lpipe[2];
                if (pipe(lpipe) < 0) {
                    info_line_emit(&edit, g_handle, "broadcast",
                                   "Failed to create pipe.");
                    line_state_reset(&edit);
                    continue;
                }
                pid_t lpid = fork();
                if (lpid < 0) {
                    close(lpipe[0]); close(lpipe[1]);
                    info_line_emit(&edit, g_handle, "broadcast",
                                   "Failed to fork.");
                    line_state_reset(&edit);
                    continue;
                }
                if (lpid == 0) {
                    close(lpipe[0]);
                    dup2(lpipe[1], STDOUT_FILENO);
                    int dn = open("/dev/null", O_WRONLY);
                    if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
                    close(lpipe[1]);
                    execlp("nbs-ts", "nbs-ts", "list", (char *)NULL);
                    _exit(127);
                }
                close(lpipe[1]);

                char lout[32768];
                size_t ltotal = 0;
                ssize_t lr;
                while (ltotal < sizeof(lout) - 1 &&
                       (lr = read(lpipe[0], lout + ltotal,
                                  sizeof(lout) - 1 - ltotal)) > 0)
                    ltotal += (size_t)lr;
                lout[ltotal] = '\0';
                close(lpipe[0]);
                waitpid(lpid, NULL, 0);

                /* Parse: each line is handle\tstatus\tname\t...
                 * Match alive sessions whose name contains the tag. */
                char handles[64][16]; /* up to 64 session handles */
                int nsessions = 0;
                char *lsave = NULL;
                for (char *line = strtok_r(lout, "\n", &lsave);
                     line && nsessions < 64;
                     line = strtok_r(NULL, "\n", &lsave)) {
                    if (!strstr(line, "alive")) continue;
                    if (!strstr(line, btag)) continue;
                    /* Skip infrastructure workers (pythia, librarian, etc.) */
                    if (strstr(line, "pythia-") || strstr(line, "librarian-") ||
                        strstr(line, "fixup-") || strstr(line, "shepard-") ||
                        strstr(line, "chatdigest-"))
                        continue;
                    /* Extract handle (first tab-delimited field) */
                    size_t hlen = 0;
                    while (line[hlen] && line[hlen] != '\t') hlen++;
                    if (hlen == 0 || hlen >= sizeof(handles[0])) continue;
                    memcpy(handles[nsessions], line, hlen);
                    handles[nsessions][hlen] = '\0';
                    nsessions++;
                }

                if (nsessions == 0) {
                    line_state_reset(&edit);
                    info_line_emit(&edit, g_handle, "broadcast",
                                   "No active agents found for this project.");
                    continue;
                }

                /* Send to each session via direct FIFO write with
                 * bracketed paste (required for Claude Code's TUI). */
                const char *home = getenv("HOME");
                int sent = 0;
                size_t bmsg_len = strlen(bmsg);
                for (int si = 0; si < nsessions; si++) {
                    char fifo[4096];
                    snprintf(fifo, sizeof(fifo),
                             "%s/.nbs-ts/sessions/%s/input.fifo",
                             home ? home : "/tmp", handles[si]);

                    int fd = open(fifo, O_WRONLY | O_NONBLOCK);
                    if (fd < 0) continue;
                    int fl = fcntl(fd, F_GETFL);
                    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

                    static const char ps[] = "\x1b[200~";
                    static const char pe[] = "\x1b[201~";
                    write(fd, ps, sizeof(ps) - 1);
                    write(fd, bmsg, bmsg_len);
                    write(fd, pe, sizeof(pe) - 1);
                    close(fd);

                    usleep(100000);

                    fd = open(fifo, O_WRONLY | O_NONBLOCK);
                    if (fd < 0) { sent++; continue; }
                    fl = fcntl(fd, F_GETFL);
                    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
                    write(fd, "\r", 1);
                    close(fd);
                    sent++;
                }

                line_state_reset(&edit);
                char rmsg[128];
                snprintf(rmsg, sizeof(rmsg),
                         "Sent to %d/%d agents.", sent, nsessions);
                info_line_emit(&edit, g_handle, "broadcast", rmsg);
                continue;
            }

            /* Unknown slash command — refuse to send. Emit an out-of-chat
             * warning so the typo (e.g. `/dsa`) does not pollute chat. */
            if (edit.buf[0] == '/' && !is_known_command(edit.buf)) {
                size_t wlen = 0;
                while (edit.buf[wlen] && edit.buf[wlen] != ' ' &&
                       edit.buf[wlen] != '\t')
                    wlen++;
                if (wlen > 80) wlen = 80;
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "Unknown command: %.*s  (type /help for the list)",
                         (int)wlen, edit.buf);
                info_line_emit(&edit, g_handle, "system", msg);
                line_state_reset(&edit);
                continue;
            }

            /* Regular message: send immediately */
            send_and_display(&edit, saved_cursor_row);
            line_state_reset(&edit);
            /* Check for messages after sending */
            if (!poll_and_display(&edit, g_handle))
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
            send_and_display(&edit, 0);
            break;
        }

        /* Ctrl-C */
        if (c == 3) {
            g_quit = 1;
            if (edit.len > 0) {
                printf("\n");
                send_and_display(&edit, 0);
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

        /* Tab: slash command completion */
        if (c == '\t') {
            if (edit.len >= 2 && edit.buf[0] == '/') {
                /* Try unique completion first */
                const char *comp = find_completion(edit.buf, edit.len);
                if (comp) {
                    /* Unique match — fill it in */
                    size_t clen = strlen(comp);
                    line_ensure_cap(&edit, clen);
                    memcpy(edit.buf, comp, clen);
                    edit.buf[clen] = '\0';
                    edit.len = clen;
                    edit.cursor = clen;
                    line_redraw(&edit, g_handle);
                } else {
                    /* Ambiguous — collect and display all matches */
                    const char *matches[64];
                    int match_count = 0;
                    for (const char **cmd = g_commands; *cmd; cmd++) {
                        if (strncmp(*cmd, edit.buf, edit.len) == 0 &&
                            match_count < 64)
                            matches[match_count++] = *cmd;
                    }
                    if (match_count > 1) {
                        printf("\r\n");
                        for (int mi = 0; mi < match_count; mi++)
                            printf("  %s%s%s", DIM, matches[mi], RESET);
                        printf("\r\n");
                        print_prompt(g_handle);
                        line_redraw(&edit, g_handle);
                    }
                }
            }
            continue;
        }

        /* Ignore other control chars */
        if (c < 32) continue;

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
