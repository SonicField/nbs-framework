/*
 * md_terminal.c — Raw mode, SIGWINCH, key reading implementation.
 *
 * Keyboard input is read from /dev/tty (not stdin) so the viewer
 * works when stdin is redirected from a file or pipe:
 *   cat file.md | nbs-md-viewer
 *   nbs-md-viewer < file.md
 */

#define _POSIX_C_SOURCE 200809L

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

#include "md_terminal.h"
#include "../nbs-common/nbs_assert.h"

static struct termios orig_termios;
static int raw_active = 0;
static int tty_fd = -1;  /* fd for /dev/tty — keyboard input */
static volatile sig_atomic_t resize_flag = 0;

/* ---- signal handlers ---- */

static void sigwinch_handler(int sig)
{
    (void)sig;
    resize_flag = 1;
}

static void fatal_signal_handler(int sig)
{
    (void)sig;
    md_terminal_leave_raw();
    _exit(0);
}

/* ---- public API ---- */

int md_terminal_enter_raw(void)
{
    struct termios raw;
    struct sigaction sa;

    /* Open /dev/tty for keyboard input — works even when stdin is a pipe. */
    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd == -1)
        return -1;

    if (tcgetattr(tty_fd, &orig_termios) == -1) {
        close(tty_fd);
        tty_fd = -1;
        return -1;
    }

    raw = orig_termios;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(tty_fd, TCSAFLUSH, &raw) == -1) {
        close(tty_fd);
        tty_fd = -1;
        return -1;
    }

    /* Alternate screen, hide cursor. */
    if (write(STDOUT_FILENO, "\033[?1049h", 8) == -1 ||
        write(STDOUT_FILENO, "\033[?25l", 6) == -1) {
        tcsetattr(tty_fd, TCSAFLUSH, &orig_termios);
        close(tty_fd);
        tty_fd = -1;
        return -1;
    }

    /* SIGWINCH — only sets a flag, no I/O. */
    sa.sa_handler = sigwinch_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);

    /* Fatal signals — clean up terminal before exit. */
    sa.sa_handler = fatal_signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    atexit(md_terminal_leave_raw);
    raw_active = 1;
    return 0;
}

void md_terminal_leave_raw(void)
{
    if (!raw_active)
        return;
    raw_active = 0;

    /* Show cursor, leave alternate screen. */
    write(STDOUT_FILENO, "\033[?25h", 6);
    write(STDOUT_FILENO, "\033[?1049l", 8);

    if (tty_fd >= 0) {
        tcsetattr(tty_fd, TCSAFLUSH, &orig_termios);
        close(tty_fd);
        tty_fd = -1;
    }
}

md_key_t md_terminal_read_key(void)
{
    unsigned char c;
    ssize_t n;

    int fd = (tty_fd >= 0) ? tty_fd : STDIN_FILENO;

    for (;;) {
        n = read(fd, &c, 1);
        if (n == 1) break;
        if (n == 0) return MD_KEY_QUIT;  /* EOF */
        /* n == -1: check if interrupted by signal (e.g. SIGWINCH) */
        if (errno == EINTR) {
            if (resize_flag) return MD_KEY_UNKNOWN; /* let main loop handle resize */
            continue; /* retry */
        }
        return MD_KEY_QUIT; /* real error */
    }

    if (c == 'h' || c == 'H' || c == '?')
        return MD_KEY_HELP;

    if (c == '\r' || c == '\n')
        return MD_KEY_ENTER;

    if (c == 0x1b) {
        unsigned char seq[2];

        if (read(fd, &seq[0], 1) != 1)
            return MD_KEY_QUIT;  /* bare ESC — exit */
        if (seq[0] != '[')
            return MD_KEY_UNKNOWN;
        if (read(fd, &seq[1], 1) != 1)
            return MD_KEY_UNKNOWN;

        switch (seq[1]) {
        case 'A': return MD_KEY_UP;
        case 'B': return MD_KEY_DOWN;
        case 'C': return MD_KEY_RIGHT;
        case 'D': return MD_KEY_LEFT;
        case 'H': return MD_KEY_HOME;
        case 'F': return MD_KEY_END;
        case '5': {
            unsigned char tilde;
            if (read(fd, &tilde, 1) == 1 && tilde == '~')
                return MD_KEY_PAGE_UP;
            return MD_KEY_UNKNOWN;
        }
        case '6': {
            unsigned char tilde;
            if (read(fd, &tilde, 1) == 1 && tilde == '~')
                return MD_KEY_PAGE_DOWN;
            return MD_KEY_UNKNOWN;
        }
        case '1': {
            unsigned char tilde;
            if (read(fd, &tilde, 1) == 1 && tilde == '~')
                return MD_KEY_HOME;
            return MD_KEY_UNKNOWN;
        }
        case '4': {
            unsigned char tilde;
            if (read(fd, &tilde, 1) == 1 && tilde == '~')
                return MD_KEY_END;
            return MD_KEY_UNKNOWN;
        }
        default:
            return MD_KEY_UNKNOWN;
        }
    }

    return MD_KEY_UNKNOWN;
}

int md_terminal_get_size(int *rows, int *cols)
{
    struct winsize ws;

    ASSERT_MSG(rows != NULL, "md_terminal_get_size: rows is NULL");
    ASSERT_MSG(cols != NULL, "md_terminal_get_size: cols is NULL");

    /* Try tty_fd first, then /dev/tty directly, then STDOUT as last resort */
    int fd = (tty_fd >= 0) ? tty_fd : STDOUT_FILENO;
    if (ioctl(fd, TIOCGWINSZ, &ws) == -1) {
        /* STDOUT may be piped; try /dev/tty directly */
        int tmpfd = open("/dev/tty", O_RDONLY);
        if (tmpfd >= 0) {
            int r = ioctl(tmpfd, TIOCGWINSZ, &ws);
            close(tmpfd);
            if (r == -1) return -1;
        } else {
            return -1;
        }
    }

    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

int md_terminal_resize_pending(void)
{
    if (resize_flag) {
        resize_flag = 0;
        return 1;
    }
    return 0;
}
