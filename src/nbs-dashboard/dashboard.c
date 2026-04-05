/*
 * dashboard.c — NBS team dashboard: data collection, layout, rendering, input.
 *
 * Environment variables (for test isolation):
 *   NBS_DASHBOARD_SESSION_PREFIX  Session name prefix (default: "nbs")
 *   NBS_DASHBOARD_SIDECAR_CMD    Sidecar command name (default: "nbs-sidecar")
 */

/* Feature macros provided by Makefile: -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "dashboard.h"
#include "nbs_term_attr.h"
#include "nbs_ts_wcwidth.h"

/* ------------------------------------------------------------------ */
/* Box-drawing characters (UTF-8)                                      */
/* Double-line outer frame, single horizontal header separator,        */
/* with proper double-to-single joins.                                 */
/* ------------------------------------------------------------------ */

/* Double-line frame */
#define BOX_TL  "\xe2\x95\x94"  /* ╔ U+2554 */
#define BOX_TR  "\xe2\x95\x97"  /* ╗ U+2557 */
#define BOX_BL  "\xe2\x95\x9a"  /* ╚ U+255A */
#define BOX_BR  "\xe2\x95\x9d"  /* ╝ U+255D */
#define BOX_DH  "\xe2\x95\x90"  /* ═ U+2550 */
#define BOX_DV  "\xe2\x95\x91"  /* ║ U+2551 */

/* Double-horizontal T-joins for top/bottom borders */
#define BOX_DTD "\xe2\x95\xa4"  /* ╤ U+2564 — double-H meets single-V down */
#define BOX_DTU "\xe2\x95\xa7"  /* ╧ U+2567 — double-H meets single-V up   */

/* Single-line for internal separators */
#define BOX_H   "\xe2\x94\x80"  /* ─ U+2500 */
#define BOX_V   "\xe2\x94\x82"  /* │ U+2502 */

/* Double-to-single joins for header separator row */
#define BOX_HSL "\xe2\x95\x9f"  /* ╟ U+255F — double-V meets single-H left  */
#define BOX_HSR "\xe2\x95\xa2"  /* ╢ U+2562 — double-V meets single-H right */
#define BOX_HSX "\xe2\x94\xbc"  /* ┼ U+253C — single cross                  */

#define INDICATOR "\xe2\x96\xb8"  /* U+25B8  ▸ */
#define EM_DASH   "\xe2\x80\x94"  /* U+2014  — */
#define ARROW_UD  "\xe2\x86\x91\xe2\x86\x93"  /* ↑↓ */

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAX_AGENTS     7
#define NUM_COLS       6
#define COL_AGENT     14
#define COL_STATUS    10
#define COL_SIDECAR   10
#define COL_CURSOR    10
#define COL_LASTPOST  11
#define FIXED_WIDTH   (COL_AGENT + COL_STATUS + COL_SIDECAR + COL_CURSOR + COL_LASTPOST)
#define SEPARATORS    (NUM_COLS + 1)
#define MIN_ACTIVITY  10
#define MAX_OUTPUT    (256 * 1024)
#define REFRESH_TICKS 20   /* 20 x 100ms = 2s */

static const char *AGENT_NAMES[MAX_AGENTS] = {
    "supervisor", "generalist", "gatekeeper",
    "theologian", "testkeeper", "scribe", "medic"
};

static const int COL_WIDTHS[5] = {
    COL_AGENT, COL_STATUS, COL_SIDECAR, COL_CURSOR, COL_LASTPOST
};

/* ------------------------------------------------------------------ */
/* Key types                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    KEY_NONE = 0,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END,
    KEY_ENTER, KEY_ESCAPE,
    KEY_REFRESH, KEY_FOLLOW, KEY_SORT, KEY_HELP,
    KEY_UNKNOWN
} dash_key_t;

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[64];
    int  alive;
    int  sidecar_ok;
    int  cursor_pos;
    int  behind;
    char session_handle[64];
    char activity[256];
    char last_post[32];
} agent_row_t;

/* Simple escape-sequence stripper for raw terminal output. */
static void strip_escapes(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && j < outsz - 1) {
        if (*p == 0x1b) {
            p++;
            if (*p == '[') {
                p++;
                while (*p && !isalpha(*p) && *p != '~' && *p != '@') p++;
                if (*p) p++;
            } else if (*p == ']') {
                while (*p && *p != 0x07) p++;
                if (*p) p++;
            }
            continue;
        }
        if (*p >= 0x20 || *p == '\n')
            out[j++] = (char)*p;
        p++;
    }
    out[j] = '\0';
}

typedef enum { MODE_OVERVIEW, MODE_DETAIL } dash_mode_t;

struct dashboard {
    agent_row_t agents[MAX_AGENTS];
    int         agent_count;
    int         total_messages;
    int         paused;
    int         sidecar_count;
    int         selected;
    dash_mode_t mode;

    /* differential redraw */
    agent_row_t prev_agents[MAX_AGENTS];
    int         prev_total_messages;
    int         prev_paused;
    int         prev_sidecar_count;
    int         prev_selected;
    int         first_render;

    /* horizontal scroll */
    int         h_offset;          /* overview horizontal pan */
    int         detail_h_offset;   /* detail view horizontal pan */

    /* detail view */
    char   *detail_buf;
    char  **detail_lines;
    int     detail_line_count;
    int     detail_scroll;

    /* terminal */
    int rows, cols;

    /* paths & config */
    char nbs_root[PATH_MAX];
    char chat_file[PATH_MAX];
    char project_tag[64];
    char bin_dir[PATH_MAX];
};

/* ------------------------------------------------------------------ */
/* Global terminal state                                               */
/* ------------------------------------------------------------------ */

static struct termios g_orig_termios;
static int g_tty_fd    = -1;
static int g_raw_active = 0;
static volatile sig_atomic_t g_resize_flag = 0;

static void sigwinch_handler(int sig) { (void)sig; g_resize_flag = 1; }

static void fatal_handler(int sig)
{
    (void)sig;
    if (g_raw_active) {
        write(STDOUT_FILENO, "\033[?25h", 6);
        write(STDOUT_FILENO, "\033[?1049l", 8);
        if (g_tty_fd >= 0)
            tcsetattr(g_tty_fd, TCSAFLUSH, &g_orig_termios);
    }
    _exit(0);
}

/* ------------------------------------------------------------------ */
/* Terminal management                                                 */
/* ------------------------------------------------------------------ */

static int enter_raw(void)
{
    struct termios raw;
    struct sigaction sa;

    g_tty_fd = open("/dev/tty", O_RDWR);
    if (g_tty_fd < 0) return -1;

    if (tcgetattr(g_tty_fd, &g_orig_termios) < 0) {
        close(g_tty_fd); g_tty_fd = -1; return -1;
    }

    raw = g_orig_termios;
    cfmakeraw(&raw);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;   /* 100ms timeout */

    if (tcsetattr(g_tty_fd, TCSAFLUSH, &raw) < 0) {
        close(g_tty_fd); g_tty_fd = -1; return -1;
    }

    write(STDOUT_FILENO, "\033[?1049h", 8);  /* alternate screen */
    write(STDOUT_FILENO, "\033[?25l", 6);    /* hide cursor */
    write(STDOUT_FILENO, "\033[?7l", 5);     /* disable auto-wrap */

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = 0;
    sa.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &sa, NULL);

    sa.sa_handler = fatal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    g_raw_active = 1;
    return 0;
}

static void leave_raw(void)
{
    if (!g_raw_active) return;
    g_raw_active = 0;

    write(STDOUT_FILENO, "\033[?7h", 5);     /* re-enable auto-wrap */
    write(STDOUT_FILENO, "\033[?25h", 6);
    write(STDOUT_FILENO, "\033[?1049l", 8);

    if (g_tty_fd >= 0) {
        tcsetattr(g_tty_fd, TCSAFLUSH, &g_orig_termios);
        close(g_tty_fd);
        g_tty_fd = -1;
    }
}

static int get_term_size(int *rows, int *cols)
{
    struct winsize ws;
    int fd = (g_tty_fd >= 0) ? g_tty_fd : STDOUT_FILENO;
    if (ioctl(fd, TIOCGWINSZ, &ws) < 0) return -1;
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

static int resize_pending(void)
{
    if (g_resize_flag) { g_resize_flag = 0; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Key reading                                                         */
/* ------------------------------------------------------------------ */

static dash_key_t read_key(void)
{
    unsigned char c;
    int fd = (g_tty_fd >= 0) ? g_tty_fd : STDIN_FILENO;

    ssize_t n = read(fd, &c, 1);
    if (n == 0)  return KEY_NONE;
    if (n <  0)  return (errno == EINTR) ? KEY_NONE : KEY_ESCAPE;

    switch (c) {
    /* q removed — ESC is the sole exit key */
    case 'r': case 'R': return KEY_REFRESH;
    case 'f': case 'F': return KEY_FOLLOW;
    case 's': case 'S': return KEY_SORT;
    case '?':           return KEY_HELP;
    case '\r': case '\n': return KEY_ENTER;
    }

    if (c == 0x1b) {
        unsigned char seq[2];
        if (read(fd, &seq[0], 1) != 1) return KEY_ESCAPE;
        if (seq[0] != '[')             return KEY_ESCAPE;
        if (read(fd, &seq[1], 1) != 1) return KEY_ESCAPE;

        switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case '5': { unsigned char t;
                    return (read(fd, &t, 1) == 1 && t == '~')
                           ? KEY_PAGE_UP : KEY_UNKNOWN; }
        case '6': { unsigned char t;
                    return (read(fd, &t, 1) == 1 && t == '~')
                           ? KEY_PAGE_DOWN : KEY_UNKNOWN; }
        case '1': { unsigned char t;
                    return (read(fd, &t, 1) == 1 && t == '~')
                           ? KEY_HOME : KEY_UNKNOWN; }
        case '4': { unsigned char t;
                    return (read(fd, &t, 1) == 1 && t == '~')
                           ? KEY_END : KEY_UNKNOWN; }
        default:  return KEY_UNKNOWN;
        }
    }

    return KEY_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Run a shell command and capture stdout.  Returns the exit code. */
static int run_cmd(const char *cmd, char *out, size_t outsz)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) { out[0] = '\0'; return -1; }

    size_t total = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF && total < outsz - 1)
        out[total++] = (char)ch;
    out[total] = '\0';

    int status = pclose(fp);

    /* strip trailing whitespace */
    while (total > 0 && (out[total-1] == '\n' || out[total-1] == '\r'
                      || out[total-1] == ' '))
        out[--total] = '\0';

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Decode one UTF-8 codepoint, advance *pp, return the codepoint. */
static uint32_t utf8_decode(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    uint32_t cp;
    int bytes;

    if (*p < 0x80)       { cp = *p;                         bytes = 1; }
    else if (*p < 0xC0)  { cp = 0xFFFD;                     bytes = 1; }
    else if (*p < 0xE0)  { cp = *p & 0x1F;                  bytes = 2; }
    else if (*p < 0xF0)  { cp = *p & 0x0F;                  bytes = 3; }
    else                 { cp = *p & 0x07;                  bytes = 4; }

    for (int i = 1; i < bytes && p[i]; i++)
        cp = (cp << 6) | (p[i] & 0x3F);

    *pp = p + bytes;
    return cp;
}

/* Display width of a UTF-8 string using nbs_ts_wcwidth. */
static int utf8_dw(const char *s)
{
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        int cw = nbs_ts_wcwidth(cp);
        if (cw > 0) w += cw;
    }
    return w;
}

/* Print s padded/truncated to exactly width display columns. */
static void fprint_pad(FILE *out, const char *s, int width)
{
    int dw = utf8_dw(s);
    if (dw <= width) {
        fputs(s, out);
        int pad = width - dw;
        if (pad > 0) fprintf(out, "%*s", pad, "");
    } else {
        const unsigned char *p = (const unsigned char *)s;
        int w = 0;
        while (*p && w < width) {
            const unsigned char *before = p;
            uint32_t cp = utf8_decode(&p);
            int cw = nbs_ts_wcwidth(cp);
            if (cw <= 0) cw = 1;
            if (w + cw > width) break;
            fwrite(before, 1, (size_t)(p - before), out);
            w += cw;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */

static void resolve_bin_dir(dashboard_t *d)
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        char *slash = strrchr(buf, '/');
        if (slash) { *slash = '\0'; snprintf(d->bin_dir, sizeof(d->bin_dir), "%s", buf); return; }
    }
    d->bin_dir[0] = '\0';
}

static int find_chat_file(dashboard_t *d)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.nbs/chat", d->nbs_root);

    DIR *dp = opendir(dir);
    if (!dp) return -1;

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        size_t len = strlen(ent->d_name);
        /* skip archive files (e.g. phoenix-20260401-archive.chat) */
        if (strstr(ent->d_name, "-archive") != NULL) continue;
        if (len > 5 && strcmp(ent->d_name + len - 5, ".chat") == 0) {
            snprintf(d->chat_file, sizeof(d->chat_file), "%s/%s", dir, ent->d_name);
            closedir(dp);
            return 0;
        }
    }
    closedir(dp);
    return -1;
}

static void read_project_tag(dashboard_t *d)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.nbs/project-id", d->nbs_root);

    FILE *fp = fopen(path, "r");
    if (!fp) { snprintf(d->project_tag, sizeof(d->project_tag), "unknown"); return; }

    if (!fgets(d->project_tag, (int)sizeof(d->project_tag), fp))
        d->project_tag[0] = '\0';
    fclose(fp);

    /* strip trailing whitespace */
    size_t n = strlen(d->project_tag);
    while (n > 0 && (d->project_tag[n-1] == '\n' || d->project_tag[n-1] == '\r'
                  || d->project_tag[n-1] == ' '))
        d->project_tag[--n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Data collection                                                     */
/* ------------------------------------------------------------------ */

static void read_cursors(dashboard_t *d)
{
    char cursor_path[PATH_MAX];
    snprintf(cursor_path, sizeof(cursor_path), "%s.cursors", d->chat_file);

    for (int i = 0; i < d->agent_count; i++) {
        d->agents[i].cursor_pos = 0;
        d->agents[i].behind     = d->total_messages;
    }

    FILE *cfp = fopen(cursor_path, "r");
    if (!cfp) return;

    char line[256];
    while (fgets(line, sizeof(line), cfp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char *handle = p;
        size_t hlen = strlen(handle);
        while (hlen > 0 && (handle[hlen-1] == ' ' || handle[hlen-1] == '\t'))
            handle[--hlen] = '\0';

        int val = atoi(eq + 1);

        for (int i = 0; i < d->agent_count; i++) {
            if (strcmp(d->agents[i].name, handle) == 0) {
                d->agents[i].cursor_pos = val;
                d->agents[i].behind = d->total_messages - val;
                if (d->agents[i].behind < 0)
                    d->agents[i].behind = 0;
                break;
            }
        }
    }
    fclose(cfp);
}

/*
 * Collect data using fast bulk commands:
 *   - nbs-ts list --name=<prefix>  (one call, ~5ms, returns all sessions)
 *   - nbs-chat count               (one call, ~20ms)
 *   - pgrep                        (one call, ~5ms)
 *   - cursor file + pause file     (direct file reads, no subprocess)
 *
 * Total: 3 popen() calls instead of 22+. Completes in <100ms.
 */
static void collect_data(dashboard_t *d)
{
    const char *bin = d->bin_dir;
    char cmd[2048], out[8192];

    /* --- pause state (direct stat, no subprocess) --- */
    {
        char pause_path[PATH_MAX];
        struct stat st;
        snprintf(pause_path, sizeof(pause_path),
                 "%s/.nbs/control-pause", d->nbs_root);
        d->paused = (stat(pause_path, &st) == 0) ? 1 : 0;
    }

    /* --- message count (1 popen call) --- */
    if (bin[0])
        snprintf(cmd, sizeof(cmd),
                 "%s/nbs-chat count '%s' </dev/null 2>/dev/null",
                 bin, d->chat_file);
    else
        snprintf(cmd, sizeof(cmd),
                 "nbs-chat count '%s' </dev/null 2>/dev/null",
                 d->chat_file);
    if (run_cmd(cmd, out, sizeof(out)) == 0)
        d->total_messages = atoi(out);

    /* --- session status via nbs-ts list (1 popen call, ~5ms) --- */
    /* Reset all agents first */
    for (int i = 0; i < d->agent_count; i++) {
        d->agents[i].alive = 0;
        d->agents[i].session_handle[0] = '\0';
        snprintf(d->agents[i].activity, sizeof(d->agents[i].activity),
                 EM_DASH);
        snprintf(d->agents[i].last_post, sizeof(d->agents[i].last_post),
                 EM_DASH);
    }

    /*
     * List sessions. Use env var prefix if set (test isolation),
     * otherwise use project tag for fast matching.
     * Fallback: re-scan nbs- sessions matched by project root.
     */
    const char *env_pfx = getenv("NBS_DASHBOARD_SESSION_PREFIX");
    const char *list_filter = (env_pfx && env_pfx[0]) ? env_pfx : d->project_tag;

    if (bin[0])
        snprintf(cmd, sizeof(cmd),
                 "%s/nbs-ts list --name=%s </dev/null 2>/dev/null",
                 bin, list_filter);
    else
        snprintf(cmd, sizeof(cmd),
                 "nbs-ts list --name=%s </dev/null 2>/dev/null",
                 list_filter);

    if (run_cmd(cmd, out, sizeof(out)) == 0 && out[0]) {
        /* Parse: handle\tstatus\tname[\tcommand] per line.
         * Match by agent name substring "-<agent>-" in session name.
         * The --name=<tag> filter already scoped results to this project. */
        char *saveptr = NULL;
        char *line = strtok_r(out, "\n", &saveptr);
        while (line) {
            char *handle_s = line;
            char *status_s = NULL;
            char *name_s   = NULL;

            char *tab1 = strchr(handle_s, '\t');
            if (tab1) {
                *tab1 = '\0';
                status_s = tab1 + 1;
                char *tab2 = strchr(status_s, '\t');
                if (tab2) {
                    *tab2 = '\0';
                    name_s = tab2 + 1;
                    char *tab3 = strchr(name_s, '\t');
                    if (tab3) *tab3 = '\0';
                }
            }

            if (!name_s || !status_s) { line = strtok_r(NULL, "\n", &saveptr); continue; }

            for (int i = 0; i < d->agent_count; i++) {
                char needle[128];
                snprintf(needle, sizeof(needle), "-%s-",
                         d->agents[i].name);
                if (strstr(name_s, needle)) {
                    snprintf(d->agents[i].session_handle,
                             sizeof(d->agents[i].session_handle),
                             "%s", handle_s);
                    d->agents[i].alive =
                        (strstr(status_s, "alive") != NULL) ? 1 : 0;
                    if (d->agents[i].alive)
                        snprintf(d->agents[i].activity,
                                 sizeof(d->agents[i].activity), "Idle");
                    break;
                }
            }

            line = strtok_r(NULL, "\n", &saveptr);
        }
    }

    /* Second pass: if no agents found by tag, try matching by project root
     * in the command column. Handles cases where session tag differs from
     * project-id (e.g., sessions tagged "dash" vs project-id "53f1"). */
    {
        int found = 0;
        for (int i = 0; i < d->agent_count; i++)
            if (d->agents[i].session_handle[0]) { found = 1; break; }

        if (!found) {
            if (bin[0])
                snprintf(cmd, sizeof(cmd),
                         "%s/nbs-ts list --name=nbs- </dev/null 2>/dev/null",
                         bin);
            else
                snprintf(cmd, sizeof(cmd),
                         "nbs-ts list --name=nbs- </dev/null 2>/dev/null");

            if (run_cmd(cmd, out, sizeof(out)) == 0 && out[0]) {
                char *sv2 = NULL;
                char *ln2 = strtok_r(out, "\n", &sv2);
                while (ln2) {
                    char full[1024];
                    snprintf(full, sizeof(full), "%s", ln2);

                    char *h2 = ln2;
                    char *s2 = NULL, *n2 = NULL;
                    char *t1 = strchr(h2, '\t');
                    if (t1) { *t1 = '\0'; s2 = t1+1;
                        char *t2 = strchr(s2, '\t');
                        if (t2) { *t2 = '\0'; n2 = t2+1;
                            char *t3 = strchr(n2, '\t');
                            if (t3) *t3 = '\0';
                        }
                    }
                    if (!n2 || !s2) { ln2 = strtok_r(NULL, "\n", &sv2); continue; }

                    /* match by project root in full line (command column) */
                    if (strstr(full, d->nbs_root)) {
                        for (int i = 0; i < d->agent_count; i++) {
                            char needle[128];
                            snprintf(needle, sizeof(needle), "-%s-",
                                     d->agents[i].name);
                            if (strstr(n2, needle) &&
                                !d->agents[i].session_handle[0]) {
                                snprintf(d->agents[i].session_handle,
                                         sizeof(d->agents[i].session_handle),
                                         "%s", h2);
                                d->agents[i].alive =
                                    (strstr(s2, "alive") != NULL) ? 1 : 0;
                                if (d->agents[i].alive)
                                    snprintf(d->agents[i].activity,
                                             sizeof(d->agents[i].activity),
                                             "Idle");
                                break;
                            }
                        }
                    }
                    ln2 = strtok_r(NULL, "\n", &sv2);
                }
            }
        }
    }

    /* --- sidecar status via pgrep (1 popen call) --- */
    /*
     * Match sidecars by project root path in the command line.
     * This ensures we only find sidecars for THIS project, not
     * sidecars for other projects or phoenix.
     */
    d->sidecar_count = 0;
    /*
     * Sidecar discovery: match by project root in command line.
     * This ensures we find sidecars for THIS project only,
     * regardless of the sidecar binary name (nbs-sidecar, dashtest-sidecar).
     * When NBS_DASHBOARD_SIDECAR_CMD is explicitly set (not the default),
     * use that as the primary pattern for faster matching.
     */
    {
        const char *env_sc = getenv("NBS_DASHBOARD_SIDECAR_CMD");
        if (env_sc && env_sc[0])
            snprintf(cmd, sizeof(cmd),
                     "pgrep -a -f '%s.*--handle=' </dev/null 2>/dev/null",
                     env_sc);
        else
            snprintf(cmd, sizeof(cmd),
                     "pgrep -a -f 'sidecar.*%s' </dev/null 2>/dev/null",
                     d->nbs_root);
    }
    if (run_cmd(cmd, out, sizeof(out)) == 0 && out[0]) {
        for (int i = 0; i < d->agent_count; i++) {
            char pattern[128];
            snprintf(pattern, sizeof(pattern),
                     "--handle=%s", d->agents[i].name);
            if (strstr(out, pattern)) {
                d->agents[i].sidecar_ok = 1;
                d->sidecar_count++;
            } else {
                d->agents[i].sidecar_ok = 0;
            }
        }
    } else {
        for (int i = 0; i < d->agent_count; i++)
            d->agents[i].sidecar_ok = 0;
    }

    /* --- cursor file (direct file read) --- */
    read_cursors(d);

    /* --- activity: read last 4KB of output.log, strip escapes --- */
    {
        const char *home = getenv("HOME");
        if (home) {
            for (int i = 0; i < d->agent_count; i++) {
                agent_row_t *a = &d->agents[i];
                if (!a->alive || !a->session_handle[0]) continue;

                char logpath[PATH_MAX];
                snprintf(logpath, sizeof(logpath),
                         "%s/.nbs-ts/sessions/%s/output.log",
                         home, a->session_handle);

                FILE *lfp = fopen(logpath, "r");
                if (!lfp) continue;

                fseek(lfp, 0, SEEK_END);
                long sz = ftell(lfp);
                long off = sz > 4096 ? sz - 4096 : 0;
                fseek(lfp, off, SEEK_SET);

                char raw[4097];
                size_t nr = fread(raw, 1, 4096, lfp);
                raw[nr] = '\0';
                fclose(lfp);

                char clean[4097];
                strip_escapes(raw, clean, sizeof(clean));

                /* take last non-empty line */
                char *last = clean;
                for (char *p = clean; *p; p++) {
                    if (*p == '\n' && *(p + 1) && *(p + 1) != '\n')
                        last = p + 1;
                }
                /* trim leading whitespace */
                while (*last == ' ' || *last == '\t') last++;

                if (last[0]) {
                    /* remove trailing newline */
                    size_t ll = strlen(last);
                    while (ll > 0 && (last[ll-1] == '\n' || last[ll-1] == '\r'))
                        last[--ll] = '\0';
                    if (last[0])
                        snprintf(a->activity, sizeof(a->activity), "%s", last);
                }
            }
        }
    }

    /* --- last post: find most recent message timestamp per agent --- */
    /* Use a shell pipeline to extract only timestamps and handles,
     * avoiding the need to buffer full message content. */
    if (bin[0])
        snprintf(cmd, sizeof(cmd),
                 "%s/nbs-chat read '%s' --last=50 </dev/null 2>/dev/null | "
                 "grep -oE '^\\[[-0-9T:Z]+\\] [a-zA-Z_-]+:'",
                 bin, d->chat_file);
    else
        snprintf(cmd, sizeof(cmd),
                 "nbs-chat read '%s' --last=50 </dev/null 2>/dev/null | "
                 "grep -oE '^\\[[-0-9T:Z]+\\] [a-zA-Z_-]+:'",
                 d->chat_file);

    if (run_cmd(cmd, out, sizeof(out)) == 0 && out[0]) {
        time_t now_t = time(NULL);

        /* For each agent, scan backward through messages to find their
         * most recent post and compute a human-readable time delta. */
        for (int i = 0; i < d->agent_count; i++) {
            char handle_pat[128];
            snprintf(handle_pat, sizeof(handle_pat),
                     "] %s:", d->agents[i].name);

            /* find LAST occurrence of handle in output */
            char *found = NULL;
            char *search = out;
            while ((search = strstr(search, handle_pat)) != NULL) {
                found = search;
                search += strlen(handle_pat);
            }

            if (found) {
                /* walk back to find the timestamp: [YYYY-MM-DDTHH:MM:SSZ] */
                char *ts_start = found;
                while (ts_start > out && *ts_start != '[') ts_start--;

                if (*ts_start == '[') {
                    struct tm msg_tm;
                    memset(&msg_tm, 0, sizeof(msg_tm));
                    if (strptime(ts_start + 1, "%Y-%m-%dT%H:%M:%S", &msg_tm)) {
                        time_t msg_t = timegm(&msg_tm);
                        long delta = (long)(now_t - msg_t);
                        if (delta < 0) delta = 0;

                        if (delta < 60)
                            snprintf(d->agents[i].last_post,
                                     sizeof(d->agents[i].last_post),
                                     "%lds ago", delta);
                        else if (delta < 3600)
                            snprintf(d->agents[i].last_post,
                                     sizeof(d->agents[i].last_post),
                                     "%ldm ago", delta / 60);
                        else if (delta < 86400)
                            snprintf(d->agents[i].last_post,
                                     sizeof(d->agents[i].last_post),
                                     "%ldh ago", delta / 3600);
                        else
                            snprintf(d->agents[i].last_post,
                                     sizeof(d->agents[i].last_post),
                                     "%ldd ago", delta / 86400);
                    } else {
                        snprintf(d->agents[i].last_post,
                                 sizeof(d->agents[i].last_post), "recent");
                    }
                } else {
                    snprintf(d->agents[i].last_post,
                             sizeof(d->agents[i].last_post), "recent");
                }
            } else {
                snprintf(d->agents[i].last_post,
                         sizeof(d->agents[i].last_post), "never");
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Detail view data loading                                            */
/* ------------------------------------------------------------------ */

static void free_detail(dashboard_t *d)
{
    free(d->detail_buf);
    free(d->detail_lines);
    d->detail_buf        = NULL;
    d->detail_lines      = NULL;
    d->detail_line_count = 0;
    d->detail_scroll     = 0;
}

/*
 * Backward-scan for last screen clear in output.log.
 * Pattern from sidecar.c:259-299 (handle_query):
 *   1. Open output.log, seek to end
 *   2. Scan backwards up to 1MB for ESC[2J (erase display)
 *   3. Set render_start to that position
 *   4. Fallback: last 64KB if no clear found within 1MB
 *   5. Pipe from render_start through nbs-ts-render
 */
static void load_detail(dashboard_t *d)
{
    free_detail(d);

    int idx = d->selected;
    if (idx < 0 || idx >= d->agent_count) return;
    agent_row_t *a = &d->agents[idx];
    if (!a->session_handle[0]) return;

    const char *home = getenv("HOME");
    if (!home) return;

    char logpath[PATH_MAX];
    snprintf(logpath, sizeof(logpath),
             "%s/.nbs-ts/sessions/%s/output.log", home, a->session_handle);

    /* backward-scan for ESC[2J */
    long render_start = 0;
    {
        FILE *lf = fopen(logpath, "r");
        if (!lf) return;

        fseek(lf, 0, SEEK_END);
        long filesize = ftell(lf);

        /* scan back up to 1MB for ESC[2J */
        long scan_limit = filesize > (1024 * 1024) ? filesize - (1024 * 1024) : 0;
        long pos = filesize - 1;
        int found = 0;

        while (pos >= scan_limit && pos >= 3) {
            fseek(lf, pos - 3, SEEK_SET);
            unsigned char buf[4];
            if (fread(buf, 1, 4, lf) == 4) {
                /* look for ESC [ 2 J  = 0x1b 0x5b 0x32 0x4a */
                if (buf[0] == 0x1b && buf[1] == '[' && buf[2] == '2' && buf[3] == 'J') {
                    render_start = pos - 3;
                    found = 1;
                    break;
                }
            }
            pos--;
        }

        /* fallback: last 64KB */
        if (!found) {
            render_start = filesize > 65536 ? filesize - 65536 : 0;
        }

        fclose(lf);
    }

    /* pipe from render_start through nbs-ts-render */
    char cmd[1024];
    if (d->bin_dir[0])
        snprintf(cmd, sizeof(cmd),
                 "tail -c +%ld '%s' 2>/dev/null | "
                 "%s/nbs-ts-render --no-strip --width=%d --height=%d 2>/dev/null",
                 render_start + 1, logpath,
                 d->bin_dir, d->cols, d->rows - 3);
    else
        snprintf(cmd, sizeof(cmd),
                 "tail -c +%ld '%s' 2>/dev/null | "
                 "nbs-ts-render --no-strip --width=%d --height=%d 2>/dev/null",
                 render_start + 1, logpath,
                 d->cols, d->rows - 3);

    d->detail_buf = malloc(MAX_OUTPUT);
    if (!d->detail_buf) return;

    FILE *fp = popen(cmd, "r");
    if (!fp) { free(d->detail_buf); d->detail_buf = NULL; return; }

    size_t total = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF && total < (size_t)MAX_OUTPUT - 1)
        d->detail_buf[total++] = (char)ch;
    d->detail_buf[total] = '\0';
    pclose(fp);

    /* split into lines */
    int nlines = 1;
    for (size_t i = 0; i < total; i++)
        if (d->detail_buf[i] == '\n') nlines++;

    d->detail_lines = calloc((size_t)nlines, sizeof(char *));
    if (!d->detail_lines) return;

    d->detail_lines[0] = d->detail_buf;
    int li = 1;
    for (size_t i = 0; i < total; i++) {
        if (d->detail_buf[i] == '\n') {
            d->detail_buf[i] = '\0';
            if (i + 1 < total && li < nlines)
                d->detail_lines[li++] = d->detail_buf + i + 1;
        }
    }
    d->detail_line_count = li;
}

/* ------------------------------------------------------------------ */
/* Rendering helpers                                                   */
/* ------------------------------------------------------------------ */

static void hborder_to(FILE *out, const char *left, const char *fill,
                       const char *mid,  const char *right,
                       const int *widths)
{
    fputs(left, out);
    for (int c = 0; c < NUM_COLS; c++) {
        for (int w = 0; w < widths[c]; w++) fputs(fill, out);
        fputs((c < NUM_COLS - 1) ? mid : right, out);
    }
}

/* ------------------------------------------------------------------ */
/* Rendering: overview                                                 */
/* ------------------------------------------------------------------ */

/*
 * Print a string with horizontal offset and width clipping.
 * Skips the first h_offset display columns and outputs at most
 * width display columns. Simple byte-level implementation that
 * works correctly for ASCII and most UTF-8 content.
 */
/*
 * Skip one ANSI escape sequence starting at p (after the ESC byte).
 * Handles CSI (ESC[...letter), OSC (ESC]...BEL/ST), and SS2/SS3.
 * Returns pointer past the end of the sequence.
 */
static const unsigned char *skip_esc(const unsigned char *p)
{
    if (*p == '[') {
        /* CSI: ESC [ (params) (intermediates) final_byte */
        p++;
        while (*p >= 0x30 && *p <= 0x3F) p++;  /* parameter bytes */
        while (*p >= 0x20 && *p <= 0x2F) p++;  /* intermediate bytes */
        if (*p >= 0x40 && *p <= 0x7E) p++;      /* final byte */
    } else if (*p == ']') {
        /* OSC: ESC ] ... (BEL | ESC \) */
        p++;
        while (*p && *p != 0x07 && !(*p == 0x1b && *(p+1) == '\\')) p++;
        if (*p == 0x07) p++;
        else if (*p == 0x1b && *(p+1) == '\\') p += 2;
    } else if (*p == 'O' || *p == 'N') {
        /* SS3/SS2: ESC O/N + one byte */
        p++;
        if (*p) p++;
    } else if (*p >= 0x40 && *p <= 0x7E) {
        /* Two-char sequence: ESC + final */
        p++;
    }
    return p;
}

static void fprint_hoffset(FILE *out, const char *s, int h_offset, int width)
{
    const unsigned char *p = (const unsigned char *)s;
    int col = 0;

    /* skip h_offset display columns */
    while (*p && col < h_offset) {
        if (*p == 0x1b) {
            /* emit escape sequences during skip (preserves colour state) */
            const unsigned char *start = p;
            p++;
            p = skip_esc(p);
            fwrite(start, 1, (size_t)(p - start), out);
            continue;
        }
        if (*p < 0x20) { p++; continue; }  /* skip control chars */
        uint32_t cp = utf8_decode(&p);
        int cw = nbs_ts_wcwidth(cp);
        if (cw > 0) col += cw;
    }

    /* output up to width display columns */
    int out_col = 0;
    while (*p && out_col < width) {
        if (*p == 0x1b) {
            const unsigned char *start = p;
            p++;
            p = skip_esc(p);
            fwrite(start, 1, (size_t)(p - start), out);
            continue;
        }
        if (*p < 0x20) { p++; continue; }  /* skip control chars */
        const unsigned char *before = p;
        uint32_t cp = utf8_decode(&p);
        int cw = nbs_ts_wcwidth(cp);
        if (cw <= 0) cw = 1;
        if (out_col + cw > width) break;
        fwrite(before, 1, (size_t)(p - before), out);
        out_col += cw;
    }
}

/* Position cursor at a specific row (1-based) */
static void goto_row(int row)
{
    printf("\033[%d;1H", row);
}


static void render_overview(dashboard_t *d)
{
    int col_act = d->cols - FIXED_WIDTH - SEPARATORS;
    if (col_act < MIN_ACTIVITY) col_act = MIN_ACTIVITY;

    int widths[NUM_COLS];
    for (int i = 0; i < 5; i++) widths[i] = COL_WIDTHS[i];
    widths[5] = col_act;

    nbs_style_t sty_bold = { NBS_COLOUR_NONE, NBS_COLOUR_NONE, NBS_ATTR_BOLD };
    nbs_style_t sty_dim  = { NBS_COLOUR_NONE, NBS_COLOUR_NONE, NBS_ATTR_DIM };
    nbs_style_t sty_red  = { 196, NBS_COLOUR_NONE, 0 };
    nbs_style_t sty_yel  = { 226, NBS_COLOUR_NONE, 0 };

    /*
     * Differential redraw: on first render, draw everything.
     * On subsequent renders, only update rows where data changed.
     * Use cursor positioning (\033[row;1H) for each updated row.
     */
    int full = d->first_render;
    int status_changed = full ||
        d->total_messages != d->prev_total_messages ||
        d->paused != d->prev_paused;

    /* --- line 1: header (always update — contains timestamp) --- */
    goto_row(1);
    fputs("\033[2K", stdout);
    nbs_style_fstart(&sty_bold, stdout);
    fputs("NBS DASHBOARD", stdout);
    nbs_style_freset(stdout);

    printf(" " EM_DASH " %s (%d agents, %d sidecars)",
           d->project_tag, d->agent_count, d->sidecar_count);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%d %b %H:%M:%S", tm);
    int hdr_used = 14 + 3 + (int)strlen(d->project_tag) + 30;
    int ts_pad   = d->cols - hdr_used - (int)strlen(ts);
    if (ts_pad > 0) printf("%*s", ts_pad, "");
    nbs_style_fstart(&sty_dim, stdout);
    fputs(ts, stdout);
    nbs_style_freset(stdout);

    int ho = d->h_offset;

    if (full) {
        /* --- line 2: top border --- */
        goto_row(2);
        {
            char *bbuf = NULL; size_t bsz = 0;
            FILE *bm = open_memstream(&bbuf, &bsz);
            if (bm) { hborder_to(bm, BOX_TL, BOX_DH, BOX_DTD, BOX_TR, widths); fclose(bm); }
            if (bbuf) { fprint_hoffset(stdout, bbuf, ho, d->cols); free(bbuf); }
        }

        /* --- line 3: column headers --- */
        goto_row(3);
        {
            static const char *hdrs[NUM_COLS] = {
                "Agent", "Status", "Sidecar", "Cursor", "Last Post", "Activity"
            };
            char *bbuf = NULL; size_t bsz = 0;
            FILE *bm = open_memstream(&bbuf, &bsz);
            if (bm) {
                nbs_style_fstart(&sty_bold, bm);
                for (int c = 0; c < NUM_COLS; c++) {
                    fputs((c == 0) ? BOX_DV " " : BOX_V " ", bm);
                    fprint_pad(bm, hdrs[c], widths[c] - 2);
                    fputc(' ', bm);
                }
                fputs(BOX_DV, bm);
                nbs_style_freset(bm);
                fclose(bm);
            }
            if (bbuf) { fprint_hoffset(stdout, bbuf, ho, d->cols); free(bbuf); }
        }

        /* --- line 4: header separator --- */
        goto_row(4);
        {
            char *bbuf = NULL; size_t bsz = 0;
            FILE *bm = open_memstream(&bbuf, &bsz);
            if (bm) { hborder_to(bm, BOX_HSL, BOX_H, BOX_HSX, BOX_HSR, widths); fclose(bm); }
            if (bbuf) { fprint_hoffset(stdout, bbuf, ho, d->cols); free(bbuf); }
        }
    }

    /* --- lines 5..11: agent rows (row 5 = agent 0) --- */
    for (int i = 0; i < d->agent_count; i++) {
        agent_row_t *a = &d->agents[i];
        int sel = (i == d->selected);

        /* Skip unchanged rows for differential redraw */
        if (!full &&
            memcmp(a, &d->prev_agents[i], sizeof(agent_row_t)) == 0 &&
            sel == (i == d->prev_selected))
            continue;

        goto_row(5 + i);

        const char *status_text  = a->alive      ? "alive"   : "dead";
        const char *sidecar_text = a->sidecar_ok  ? "OK"      : "MISSING";

        nbs_style_t *sty_status   = NULL;
        nbs_style_t *sty_sidecar  = NULL;
        nbs_style_t *sty_cursor   = NULL;
        nbs_style_t *sty_lastpost = NULL;

        if (!a->alive)           sty_status   = &sty_red;
        if (!a->sidecar_ok)      sty_sidecar  = &sty_red;
        if (a->behind > 50)      sty_cursor   = &sty_red;
        else if (a->behind > 10) sty_cursor   = &sty_yel;
        /* silence warning: "never" = red, anything non-"recent" = yellow */
        if (strcmp(a->last_post, "never") == 0)
            sty_lastpost = &sty_red;
        else if (strcmp(a->last_post, "recent") != 0 &&
                 strcmp(a->last_post, EM_DASH) != 0)
            sty_lastpost = &sty_yel;

        char cursor_text[32];
        snprintf(cursor_text, sizeof(cursor_text), "%d/%d",
                 a->cursor_pos, a->behind);

        /* agent name with indicator */
        char name_buf[80];
        snprintf(name_buf, sizeof(name_buf), "%s%s",
                 sel ? INDICATOR : " ", a->name);

        /* render row to buffer for h-scroll clipping */
        {
            char *rbuf = NULL; size_t rsz = 0;
            FILE *rf = open_memstream(&rbuf, &rsz);
            if (!rf) continue;

            fputs(BOX_DV " ", rf);
            if (sel) nbs_style_fstart(&sty_bold, rf);
            fprint_pad(rf, name_buf, widths[0] - 2);
            if (sel) nbs_style_freset(rf);
            fputc(' ', rf);

            fputs(BOX_V " ", rf);
            if (sty_status) nbs_style_fstart(sty_status, rf);
            fprint_pad(rf, status_text, widths[1] - 2);
            if (sty_status) nbs_style_freset(rf);
            fputc(' ', rf);

            fputs(BOX_V " ", rf);
            if (sty_sidecar) nbs_style_fstart(sty_sidecar, rf);
            fprint_pad(rf, sidecar_text, widths[2] - 2);
            if (sty_sidecar) nbs_style_freset(rf);
            fputc(' ', rf);

            fputs(BOX_V " ", rf);
            if (sty_cursor) nbs_style_fstart(sty_cursor, rf);
            fprint_pad(rf, cursor_text, widths[3] - 2);
            if (sty_cursor) nbs_style_freset(rf);
            fputc(' ', rf);

            fputs(BOX_V " ", rf);
            if (sty_lastpost) nbs_style_fstart(sty_lastpost, rf);
            fprint_pad(rf, a->last_post, widths[4] - 2);
            if (sty_lastpost) nbs_style_freset(rf);
            fputc(' ', rf);

            fputs(BOX_V " ", rf);
            fprint_pad(rf, a->activity, widths[5] - 2);
            fputc(' ', rf);

            fputs(BOX_DV, rf);
            fclose(rf);

            if (rbuf) {
                fprint_hoffset(stdout, rbuf, ho, d->cols);
                free(rbuf);
            }
        }
    }

    int bot_row = 5 + d->agent_count;

    if (full) {
        /* --- bottom border --- */
        goto_row(bot_row);
        {
            char *bbuf = NULL; size_t bsz = 0;
            FILE *bm = open_memstream(&bbuf, &bsz);
            if (bm) { hborder_to(bm, BOX_BL, BOX_DH, BOX_DTU, BOX_BR, widths); fclose(bm); }
            if (bbuf) { fprint_hoffset(stdout, bbuf, ho, d->cols); free(bbuf); }
        }
    }

    if (status_changed || full) {
        /* --- status bar --- */
        goto_row(bot_row + 1);
        fputs("\033[2K", stdout);
        printf("Paused: %s   Messages: %d",
               d->paused ? "yes" : "no", d->total_messages);
    }

    if (full) {
        /* --- navigation hints --- */
        goto_row(bot_row + 2);
        fputs("\033[2K", stdout);
        nbs_style_fstart(&sty_dim, stdout);
        fputs(ARROW_UD ": select   Enter: detail   Esc: exit   r: refresh",
              stdout);
        nbs_style_freset(stdout);
        fputs("\033[J", stdout);   /* clear below */
    }

    /* save state for next differential comparison */
    memcpy(d->prev_agents, d->agents, sizeof(d->agents));
    d->prev_total_messages = d->total_messages;
    d->prev_paused         = d->paused;
    d->prev_sidecar_count  = d->sidecar_count;
    d->prev_selected       = d->selected;
    d->first_render        = 0;

    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Rendering: detail                                                   */
/* ------------------------------------------------------------------ */

static void render_detail(dashboard_t *d)
{
    int idx = d->selected;
    if (idx < 0 || idx >= d->agent_count) return;
    agent_row_t *a = &d->agents[idx];

    nbs_style_t sty_bold = { NBS_COLOUR_NONE, NBS_COLOUR_NONE, NBS_ATTR_BOLD };
    nbs_style_t sty_dim  = { NBS_COLOUR_NONE, NBS_COLOUR_NONE, NBS_ATTR_DIM };

    fputs("\033[H", stdout);

    /* header */
    fputs("\033[2K", stdout);
    nbs_style_fstart(&sty_bold, stdout);
    fputs(a->name, stdout);
    nbs_style_freset(stdout);
    if (a->session_handle[0])
        printf(" " EM_DASH " session %s", a->session_handle);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%d %b %H:%M:%S", tm);
    int left_len = (int)strlen(a->name) + 12 + (int)strlen(a->session_handle);
    int pad = d->cols - left_len - (int)strlen(ts);
    if (pad > 0) printf("%*s", pad, "");
    nbs_style_fstart(&sty_dim, stdout);
    fputs(ts, stdout);
    nbs_style_freset(stdout);
    fputs("\r\n", stdout);

    /* separator */
    for (int c = 0; c < d->cols; c++) fputs(BOX_H, stdout);
    fputs("\r\n", stdout);

    /* content area */
    int content_rows = d->rows - 4;
    if (content_rows < 1) content_rows = 1;

    int max_scroll = d->detail_line_count - content_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (d->detail_scroll > max_scroll) d->detail_scroll = max_scroll;
    if (d->detail_scroll < 0) d->detail_scroll = 0;

    for (int r = 0; r < content_rows; r++) {
        int li = d->detail_scroll + r;
        fputs("\033[2K", stdout);
        if (li < d->detail_line_count && d->detail_lines) {
            fprint_hoffset(stdout, d->detail_lines[li],
                           d->detail_h_offset, d->cols);
            fputs("\033[0m", stdout);  /* reset SGR to prevent colour bleed */
        }
        fputs("\r\n", stdout);
    }

    /* navigation */
    fputs("\033[2K", stdout);
    nbs_style_fstart(&sty_dim, stdout);
    fputs("Esc: back   " ARROW_UD ": scroll   PgUp/PgDn: page   "
          "r: refresh   f: follow", stdout);
    nbs_style_freset(stdout);
    fputs("\033[K", stdout);

    fputs("\033[J", stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Rendering dispatch                                                  */
/* ------------------------------------------------------------------ */

static void render(dashboard_t *d)
{
    if (d->mode == MODE_OVERVIEW) render_overview(d);
    else                          render_detail(d);
}

/* ------------------------------------------------------------------ */
/* Event handling                                                      */
/* ------------------------------------------------------------------ */

/* Returns 1 if the dashboard should exit. */
static int handle_key(dashboard_t *d, dash_key_t key)
{
    if (d->mode == MODE_OVERVIEW) {
        switch (key) {
        case KEY_ESCAPE:
            return 1;
        case KEY_UP:
            if (d->selected > 0) d->selected--;
            break;
        case KEY_DOWN:
            if (d->selected < d->agent_count - 1) d->selected++;
            break;
        case KEY_LEFT:
            if (d->h_offset > 0) { d->h_offset -= 4; d->first_render = 1; }
            break;
        case KEY_RIGHT:
            d->h_offset += 4; d->first_render = 1;
            break;
        case KEY_HOME:
            d->selected = 0;
            break;
        case KEY_END:
            d->selected = d->agent_count - 1;
            break;
        case KEY_ENTER:
            d->mode = MODE_DETAIL;
            d->detail_h_offset = 0;
            load_detail(d);
            break;
        case KEY_REFRESH:
            collect_data(d);
            break;
        default:
            break;
        }
    } else {
        int content_rows = d->rows - 4;
        if (content_rows < 1) content_rows = 1;

        switch (key) {
        case KEY_ESCAPE:
            d->mode = MODE_OVERVIEW;
            d->first_render = 1;  /* full redraw after leaving detail */
            free_detail(d);
            break;
        case KEY_UP:
            if (d->detail_scroll > 0) d->detail_scroll--;
            break;
        case KEY_DOWN:
            d->detail_scroll++;
            break;
        case KEY_LEFT:
            if (d->detail_h_offset > 0) d->detail_h_offset -= 4;
            break;
        case KEY_RIGHT:
            d->detail_h_offset += 4;
            break;
        case KEY_PAGE_UP:
            d->detail_scroll -= content_rows;
            if (d->detail_scroll < 0) d->detail_scroll = 0;
            break;
        case KEY_PAGE_DOWN:
            d->detail_scroll += content_rows;
            break;
        case KEY_HOME:
            d->detail_scroll = 0;
            break;
        case KEY_END:
            d->detail_scroll = d->detail_line_count;
            break;
        case KEY_REFRESH:
        case KEY_FOLLOW:
            load_detail(d);
            if (key == KEY_FOLLOW)
                d->detail_scroll = d->detail_line_count;
            break;
        default:
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

dashboard_t *dashboard_init(const char *nbs_root)
{
    dashboard_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    snprintf(d->nbs_root, sizeof(d->nbs_root), "%s", nbs_root);

    resolve_bin_dir(d);
    read_project_tag(d);

    if (find_chat_file(d) < 0) { free(d); return NULL; }

    d->agent_count = MAX_AGENTS;
    for (int i = 0; i < MAX_AGENTS; i++)
        snprintf(d->agents[i].name, sizeof(d->agents[i].name),
                 "%s", AGENT_NAMES[i]);

    d->selected     = 0;
    d->mode         = MODE_OVERVIEW;
    d->first_render = 1;
    return d;
}

void dashboard_run(dashboard_t *d)
{
    if (enter_raw() < 0) return;

    if (get_term_size(&d->rows, &d->cols) < 0) {
        d->rows = 24;
        d->cols = 80;
    }

    collect_data(d);
    render(d);

    int tick = 0;
    for (;;) {
        dash_key_t key = read_key();

        if (key == KEY_NONE) {
            tick++;

            if (resize_pending()) {
                get_term_size(&d->rows, &d->cols);
                if (d->mode == MODE_DETAIL) load_detail(d);
                render(d);
                tick = 0;
            }

            if (tick >= REFRESH_TICKS) {
                tick = 0;
                collect_data(d);
                if (d->mode == MODE_DETAIL) load_detail(d);
                render(d);
            }
            continue;
        }

        tick = 0;

        if (handle_key(d, key))
            break;

        render(d);
    }

    leave_raw();
}

void dashboard_free(dashboard_t *d)
{
    if (!d) return;
    free_detail(d);
    free(d);
}
