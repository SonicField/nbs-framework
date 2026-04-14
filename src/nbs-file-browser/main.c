/*
 * nbs-file-browser — Lightweight terminal file browser.
 *
 * Full-screen TUI for navigating directories, viewing markdown via
 * nbs-md-viewer, and editing files with $EDITOR.
 *
 * Usage:
 *   nbs-file-browser [path]     # browse from path (default: cwd)
 *
 * Keys:
 *   Up/Down      Navigate files
 *   Enter        Open file or descend into directory
 *   Tab          Jump 10 files forward
 *   Shift-Tab    Jump 10 files back
 *   Escape       Exit
 */

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

#include "nbs_term_attr.h"

/* --- Terminal state --- */

static struct termios g_orig_termios;
static int g_raw_mode = 0;
static int g_term_rows = 24;
static int g_term_cols = 80;
static volatile sig_atomic_t g_resized = 0;

static void handle_winch(int sig) {
    (void)sig;
    g_resized = 1;
}

static void update_term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        g_term_rows = ws.ws_row;
        g_term_cols = ws.ws_col;
    }
}

static void disable_raw(void) {
    if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = 0;
    }
    write(STDOUT_FILENO, "\x1b[?25h\x1b[?1049l", 15);
}

static void enable_raw(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cflag |= (unsigned)(CS8);
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_mode = 1;
    write(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l", 15);
}

/* --- Key reading --- */

enum {
    KEY_NONE = 0,
    KEY_UP = 256,
    KEY_DOWN,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_TAB = 9,
    KEY_SHIFT_TAB = 353,
    KEY_ENTER = 13,
    KEY_ESC = 27,
    KEY_BACKSPACE = 127,
};

static int read_key(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return KEY_NONE;

    if (c == '\x1b') {
        /* Use short timeout to distinguish bare ESC from escape sequence */
        struct termios cur, tmp;
        tcgetattr(STDIN_FILENO, &cur);
        tmp = cur;
        tmp.c_cc[VMIN] = 0;
        tmp.c_cc[VTIME] = 1; /* 100ms */
        tcsetattr(STDIN_FILENO, TCSANOW, &tmp);

        char seq[3];
        ssize_t nr = read(STDIN_FILENO, &seq[0], 1);
        if (nr <= 0) {
            tcsetattr(STDIN_FILENO, TCSANOW, &cur);
            return KEY_ESC; /* bare ESC */
        }

        if (seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1) {
                tcsetattr(STDIN_FILENO, TCSANOW, &cur);
                return KEY_NONE;
            }
            tcsetattr(STDIN_FILENO, TCSANOW, &cur);

            if (seq[1] >= '0' && seq[1] <= '9') {
                char seq2;
                if (read(STDIN_FILENO, &seq2, 1) != 1) return KEY_NONE;
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
                case 'C': return 'R'; /* right arrow — column right */
                case 'D': return 'L'; /* left arrow — column left */
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
                case 'Z': return KEY_SHIFT_TAB;
            }
            return KEY_NONE;
        } else if (seq[0] == 'O') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1) {
                tcsetattr(STDIN_FILENO, TCSANOW, &cur);
                return KEY_NONE;
            }
            tcsetattr(STDIN_FILENO, TCSANOW, &cur);
            switch (seq[1]) {
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
            return KEY_NONE;
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &cur);
        return KEY_NONE; /* unknown ESC sequence */
    }

    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == '\t') return KEY_TAB;
    if (c == 127 || c == 8) return KEY_BACKSPACE;

    return (unsigned char)c;
}

/* --- File entry --- */

typedef struct {
    char name[256];
    int is_dir;
    int is_executable;
    int is_hidden;
    off_t size;
} file_entry_t;

#define MAX_ENTRIES 4096

/* --- File type detection --- */

typedef enum {
    FTYPE_DIR,
    FTYPE_MARKDOWN,
    FTYPE_SOURCE,
    FTYPE_DATA,
    FTYPE_EXECUTABLE,
    FTYPE_HIDDEN,
    FTYPE_REGULAR,
} file_type_t;

static int has_ext(const char *name, const char *ext) {
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return 0;
    return strcasecmp(name + nlen - elen, ext) == 0;
}

static file_type_t classify(const file_entry_t *e) {
    if (e->is_dir) return FTYPE_DIR;
    if (e->is_hidden) return FTYPE_HIDDEN;
    if (has_ext(e->name, ".md")) return FTYPE_MARKDOWN;
    if (has_ext(e->name, ".c") || has_ext(e->name, ".h") ||
        has_ext(e->name, ".py") || has_ext(e->name, ".sh") ||
        has_ext(e->name, ".js") || has_ext(e->name, ".ts") ||
        has_ext(e->name, ".rs") || has_ext(e->name, ".go") ||
        has_ext(e->name, ".cpp") || has_ext(e->name, ".hpp"))
        return FTYPE_SOURCE;
    if (has_ext(e->name, ".json") || has_ext(e->name, ".yaml") ||
        has_ext(e->name, ".yml") || has_ext(e->name, ".toml") ||
        has_ext(e->name, ".honest") || has_ext(e->name, ".xml") ||
        has_ext(e->name, ".csv") || has_ext(e->name, ".cfg") ||
        has_ext(e->name, ".ini") || has_ext(e->name, ".conf"))
        return FTYPE_DATA;
    if (e->is_executable) return FTYPE_EXECUTABLE;
    return FTYPE_REGULAR;
}

/* Colour per type */
static void style_for_type(file_type_t ft, char *buf, size_t bufsz) {
    switch (ft) {
        case FTYPE_DIR:        snprintf(buf, bufsz, "\033[1;34m"); break; /* bold blue */
        case FTYPE_MARKDOWN:   snprintf(buf, bufsz, "\033[35m"); break;   /* magenta */
        case FTYPE_SOURCE:     snprintf(buf, bufsz, "\033[32m"); break;   /* green */
        case FTYPE_DATA:       snprintf(buf, bufsz, "\033[36m"); break;   /* cyan */
        case FTYPE_EXECUTABLE: snprintf(buf, bufsz, "\033[1;31m"); break; /* bold red */
        case FTYPE_HIDDEN:     snprintf(buf, bufsz, "\033[2m"); break;    /* dim */
        case FTYPE_REGULAR:    snprintf(buf, bufsz, "\033[33m"); break;   /* yellow */
    }
}

/* --- Binary detection --- */

static int is_binary(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) return 0;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == 0) return 1;
    }
    return 0;
}

/* --- Human-readable size --- */

static void format_size(off_t size, char *buf, size_t bufsz) {
    if (size < 1024)
        snprintf(buf, bufsz, "%4d B", (int)size);
    else if (size < 1024 * 1024)
        snprintf(buf, bufsz, "%4d K", (int)(size / 1024));
    else if (size < 1024LL * 1024 * 1024)
        snprintf(buf, bufsz, "%4d M", (int)(size / (1024 * 1024)));
    else
        snprintf(buf, bufsz, "%4d G", (int)(size / (1024LL * 1024 * 1024)));
}

/* --- Directory reading --- */

static int entry_cmp(const void *a, const void *b) {
    const file_entry_t *ea = a;
    const file_entry_t *eb = b;
    /* .. always first */
    if (strcmp(ea->name, "..") == 0) return -1;
    if (strcmp(eb->name, "..") == 0) return 1;
    /* Directories before files */
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    /* Alphabetical */
    return strcasecmp(ea->name, eb->name);
}

static int read_directory(const char *path, file_entry_t *entries, int max_entries) {
    DIR *d = opendir(path);
    if (!d) return -1;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_entries) {
        if (strcmp(ent->d_name, ".") == 0) continue;

        file_entry_t *e = &entries[count];
        snprintf(e->name, sizeof(e->name), "%s", ent->d_name);

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode);
            e->is_executable = !e->is_dir && (st.st_mode & S_IXUSR);
            e->size = e->is_dir ? 0 : st.st_size;
        } else {
            e->is_dir = 0;
            e->is_executable = 0;
            e->size = 0;
        }
        e->is_hidden = (ent->d_name[0] == '.' && strcmp(ent->d_name, "..") != 0);
        count++;
    }
    closedir(d);

    qsort(entries, (size_t)count, sizeof(file_entry_t), entry_cmp);
    return count;
}

/* --- Rendering --- */

/*
 * Compute number of columns for multi-column layout.
 * Uses the longest filename + size column + padding to determine
 * how many columns fit. Falls back to 1 column if names are long.
 */
static int compute_columns(const file_entry_t *entries, int count, int term_cols) {
    int max_name = 0;
    for (int i = 0; i < count; i++) {
        int len = (int)strlen(entries[i].name) + (entries[i].is_dir ? 1 : 0);
        if (len > max_name) max_name = len;
    }
    /* Each column needs: name + 7 (size) + 3 (gap between columns) */
    int col_width = max_name + 10;
    if (col_width < 20) col_width = 20;
    int ncols = term_cols / col_width;
    if (ncols < 1) ncols = 1;
    if (ncols > 4) ncols = 4;
    return ncols;
}

static void render(const char *dir_path, const file_entry_t *entries, int count,
                   int cursor, int scroll_top, const char *status_msg) {
    update_term_size();
    int content_rows = g_term_rows - 3; /* header + hint + status */

    int ncols = compute_columns(entries, count, g_term_cols);
    int col_width = g_term_cols / ncols;
    int items_per_page = content_rows * ncols;

    /* Adjust scroll_top to page boundary for multi-column */
    if (ncols > 1) {
        int page = cursor / items_per_page;
        scroll_top = page * items_per_page;
    }

    printf("\033[H\033[2J");

    /* Header */
    printf("\033[7m FILE BROWSER \033[0m");
    printf("\033[2m %s \033[0m", dir_path);

    char countstr[32];
    snprintf(countstr, sizeof(countstr), "%d items", count - 1);
    int hdr_used = 14 + 1 + (int)strlen(dir_path) + 1;
    int pad = g_term_cols - hdr_used - (int)strlen(countstr);
    if (pad > 0) printf("%*s", pad, "");
    printf("\033[2m%s\033[0m", countstr);
    printf("\r\n");

    /* File list — column-major order */
    for (int row = 0; row < content_rows; row++) {
        for (int col = 0; col < ncols; col++) {
            int idx = scroll_top + col * content_rows + row;

            if (idx >= count) {
                /* Empty cell */
                if (col < ncols - 1)
                    printf("%-*s", col_width, "");
                continue;
            }

            const file_entry_t *e = &entries[idx];
            int is_cur = (idx == cursor);
            file_type_t ft = classify(e);

            char style[32];
            style_for_type(ft, style, sizeof(style));

            char size_str[16];
            if (e->is_dir) {
                snprintf(size_str, sizeof(size_str), " <DIR>");
            } else {
                format_size(e->size, size_str, sizeof(size_str));
            }

            const char *indicator = e->is_dir ? "/" : "";

            /* Name truncated to fit column */
            int name_avail = col_width - 9;
            if (name_avail < 4) name_avail = 4;
            char display_name[512];
            snprintf(display_name, sizeof(display_name), "%s%s",
                     e->name, indicator);
            if ((int)strlen(display_name) > name_avail)
                display_name[name_avail] = '\0';

            if (is_cur) {
                printf("\033[7m%s%-*s%7s\033[0m",
                       style, name_avail, display_name, size_str);
            } else {
                printf("%s%-*s\033[0m\033[2m%7s\033[0m",
                       style, name_avail, display_name, size_str);
            }
            /* Gap between columns (not after last) */
            if (col < ncols - 1) printf("   ");
        }
        printf("\r\n");
    }

    /* Hint bar */
    printf("\033[%d;1H\033[2K\033[2m"
           "Arrows: navigate  Enter: view  e: edit  "
           "Tab: jump 10  r: refresh  Esc: exit"
           "\033[0m", g_term_rows - 1);

    /* Status bar (bottom) */
    {
        char status_buf[256];
        if (status_msg && status_msg[0]) {
            snprintf(status_buf, sizeof(status_buf), " %s", status_msg);
        } else {
            snprintf(status_buf, sizeof(status_buf), " %d/%d",
                     cursor + 1, count);
        }
        printf("\033[%d;1H\033[7m%-*s\033[0m",
               g_term_rows, g_term_cols, status_buf);
    }

    fflush(stdout);
}

/* --- File operations --- */

static void open_with_viewer(const char *path) {
    disable_raw();
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        execlp("nbs-md-viewer", "nbs-md-viewer", (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
    }
    enable_raw();
}

/*
 * Built-in pager for syntax-highlighted text.
 *
 * Runs bat --paging=never --color=always to get ANSI-coloured output,
 * reads it into memory, then displays it in a scrollable view with
 * the same key bindings as nbs-md-viewer (ESC/q to quit, arrows,
 * page up/down).
 */

#define PAGER_MAX_LINES 65536
#define PAGER_MAX_LINE  4096

static void open_with_bat(const char *path) {
    /* Run bat and capture its output */
    int pipefd[2];
    if (pipe(pipefd) < 0) return;

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execlp("bat", "bat", "--paging=never", "--color=always",
               "--style=numbers", path, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) { close(pipefd[0]); return; }

    /* Read all output into lines array */
    char **lines = calloc(PAGER_MAX_LINES, sizeof(char *));
    if (!lines) { close(pipefd[0]); waitpid(pid, NULL, 0); return; }

    int line_count = 0;
    char buf[PAGER_MAX_LINE];
    size_t buf_pos = 0;

    ssize_t nr;
    while ((nr = read(pipefd[0], buf + buf_pos, sizeof(buf) - buf_pos - 1)) > 0) {
        buf_pos += (size_t)nr;
        buf[buf_pos] = '\0';

        /* Split into lines */
        char *start = buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL && line_count < PAGER_MAX_LINES) {
            *nl = '\0';
            lines[line_count] = strdup(start);
            line_count++;
            start = nl + 1;
        }
        /* Move remainder to front */
        size_t remain = buf_pos - (size_t)(start - buf);
        if (remain > 0) memmove(buf, start, remain);
        buf_pos = remain;
    }
    /* Last line without newline */
    if (buf_pos > 0 && line_count < PAGER_MAX_LINES) {
        buf[buf_pos] = '\0';
        lines[line_count] = strdup(buf);
        line_count++;
    }
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    if (line_count == 0) {
        free(lines);
        return;
    }

    /* Pager loop */
    update_term_size();
    int scroll = 0;
    int pager_dirty = 1;
    const char *basename_str = strrchr(path, '/');
    basename_str = basename_str ? basename_str + 1 : path;

    /* Already in raw mode from caller, use alternate screen */
    write(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l", 15);

    while (1) {
        if (pager_dirty) {
            update_term_size();
            int content_rows = g_term_rows - 2; /* header + hint */
            printf("\033[H\033[2J");

            /* Header */
            printf("\033[7m VIEWER \033[0m \033[2m%s\033[0m", basename_str);
            char pos[48];
            snprintf(pos, sizeof(pos), "%d-%d / %d",
                     scroll + 1,
                     scroll + content_rows < line_count ?
                         scroll + content_rows : line_count,
                     line_count);
            int hdr_used = 8 + 1 + (int)strlen(basename_str);
            int pad = g_term_cols - hdr_used - (int)strlen(pos);
            if (pad > 0) printf("%*s", pad, "");
            printf("\033[2m%s\033[0m\r\n", pos);

            /* Content */
            for (int i = 0; i < content_rows; i++) {
                int li = scroll + i;
                if (li < line_count)
                    printf("%s\033[0m\r\n", lines[li]);
                else
                    printf("\033[2m~\033[0m\r\n");
            }

            /* Hint bar */
            printf("\033[%d;1H\033[2K\033[2m"
                   "Arrows/PgUp/PgDn: scroll  ESC/q: close"
                   "\033[0m", g_term_rows);
            fflush(stdout);
            pager_dirty = 0;
        }

        int key = read_key();
        if (key == KEY_NONE) continue;

        int content_rows = g_term_rows - 2;
        switch (key) {
        case KEY_ESC:
        case 'q':
            goto pager_done;
        case KEY_UP:
        case 'k':
            if (scroll > 0) { scroll--; pager_dirty = 1; }
            break;
        case KEY_DOWN:
        case 'j':
            if (scroll < line_count - content_rows)
                { scroll++; pager_dirty = 1; }
            break;
        case KEY_PAGE_UP:
            scroll -= content_rows;
            if (scroll < 0) scroll = 0;
            pager_dirty = 1;
            break;
        case KEY_PAGE_DOWN:
        case ' ':
            scroll += content_rows;
            if (scroll > line_count - content_rows)
                scroll = line_count - content_rows;
            if (scroll < 0) scroll = 0;
            pager_dirty = 1;
            break;
        case KEY_HOME:
            scroll = 0;
            pager_dirty = 1;
            break;
        case KEY_END:
            scroll = line_count - content_rows;
            if (scroll < 0) scroll = 0;
            pager_dirty = 1;
            break;
        default:
            break;
        }
    }

pager_done:
    write(STDOUT_FILENO, "\x1b[?25h\x1b[?1049l", 15);

    for (int i = 0; i < line_count; i++)
        free(lines[i]);
    free(lines);
}

static void open_with_editor(const char *path) {
    disable_raw();
    const char *editor = getenv("EDITOR");
    if (!editor || !editor[0]) editor = "vi";

    pid_t pid = fork();
    if (pid == 0) {
        execlp(editor, editor, path, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
    }
    enable_raw();
}

/* --- Main --- */

int main(int argc, char *argv[]) {
    char dir_path[PATH_MAX];
    const char *state_file = NULL;
    const char *start_path = NULL;

    /* Parse arguments: [--state-file=PATH] [path] */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--state-file=", 13) == 0) {
            state_file = argv[i] + 13;
        } else {
            start_path = argv[i];
        }
    }

    if (start_path) {
        if (!realpath(start_path, dir_path)) {
            fprintf(stderr, "Error: cannot resolve '%s': %s\n",
                    start_path, strerror(errno));
            return 1;
        }
    } else {
        if (!getcwd(dir_path, sizeof(dir_path))) {
            fprintf(stderr, "Error: getcwd failed: %s\n", strerror(errno));
            return 1;
        }
    }

    file_entry_t *entries = calloc(MAX_ENTRIES, sizeof(file_entry_t));
    if (!entries) { fprintf(stderr, "Error: malloc failed\n"); return 1; }

    int count = read_directory(dir_path, entries, MAX_ENTRIES);
    if (count < 0) {
        fprintf(stderr, "Error: cannot read '%s': %s\n",
                dir_path, strerror(errno));
        free(entries);
        return 1;
    }

    int cursor = 0;
    int scroll_top = 0;
    const char *status_msg = NULL;

    enable_raw();
    update_term_size();
    signal(SIGWINCH, handle_winch);

    int dirty = 1; /* initial draw */

    while (1) {
        int content_rows = g_term_rows - 3;
        (void)compute_columns(entries, count, g_term_cols);

        if (dirty) {
            render(dir_path, entries, count, cursor, scroll_top, status_msg);
            status_msg = NULL;
            dirty = 0;
        }

        if (g_resized) {
            g_resized = 0;
            update_term_size();
            dirty = 1;
        }

        int key = read_key();
        if (key == KEY_NONE) continue;

        dirty = 1; /* any keypress triggers redraw */

        switch (key) {
        case KEY_UP:
            if (cursor > 0) cursor--;
            break;

        case KEY_DOWN:
            if (cursor < count - 1) cursor++;
            break;

        case 'R': /* right arrow — next column */
            cursor += content_rows;
            if (cursor >= count) cursor = count - 1;
            break;

        case 'L': /* left arrow — prev column */
            cursor -= content_rows;
            if (cursor < 0) cursor = 0;
            break;

        case KEY_TAB:
            cursor += 10;
            if (cursor >= count) cursor = count - 1;
            break;

        case KEY_SHIFT_TAB:
            cursor -= 10;
            if (cursor < 0) cursor = 0;
            break;

        case KEY_PAGE_UP:
            cursor -= content_rows;
            if (cursor < 0) cursor = 0;
            break;

        case KEY_PAGE_DOWN:
            cursor += content_rows;
            if (cursor >= count) cursor = count - 1;
            break;

        case KEY_HOME:
            cursor = 0;
            scroll_top = 0;
            break;

        case KEY_END:
            cursor = count > 0 ? count - 1 : 0;
            break;

        case KEY_ENTER:
            if (cursor >= 0 && cursor < count) {
                const file_entry_t *e = &entries[cursor];
                if (e->is_dir) {
                    /* Descend into directory */
                    char new_path[PATH_MAX];
                    snprintf(new_path, sizeof(new_path), "%s/%s",
                             dir_path, e->name);
                    char resolved[PATH_MAX];
                    if (realpath(new_path, resolved)) {
                        snprintf(dir_path, sizeof(dir_path), "%s", resolved);
                        count = read_directory(dir_path, entries, MAX_ENTRIES);
                        if (count < 0) {
                            status_msg = "Cannot read directory";
                            count = 0;
                        }
                        cursor = 0;
                        scroll_top = 0;
                    }
                } else {
                    char full[PATH_MAX];
                    snprintf(full, sizeof(full), "%s/%s", dir_path, e->name);

                    if (is_binary(full)) {
                        status_msg = "Binary file — press 'e' to edit";
                    } else if (has_ext(e->name, ".md")) {
                        open_with_viewer(full);
                    } else {
                        open_with_bat(full);
                    }
                }
            }
            break;

        case 'e':
            /* Edit any file (including binary) in $EDITOR */
            if (cursor >= 0 && cursor < count && !entries[cursor].is_dir) {
                char full[PATH_MAX];
                snprintf(full, sizeof(full), "%s/%s",
                         dir_path, entries[cursor].name);
                open_with_editor(full);
            }
            break;

        case 'r':
            /* Refresh */
            count = read_directory(dir_path, entries, MAX_ENTRIES);
            if (count < 0) count = 0;
            if (cursor >= count) cursor = count > 0 ? count - 1 : 0;
            status_msg = "Refreshed";
            break;

        case KEY_ESC:
            goto done;

        default:
            break;
        }
    }

done:
    disable_raw();

    /* Write final directory to state file so caller can remember it */
    if (state_file) {
        FILE *sf = fopen(state_file, "w");
        if (sf) {
            fprintf(sf, "%s\n", dir_path);
            fclose(sf);
        }
    }

    free(entries);
    return 0;
}
