/*
 * main.c — Entry point for nbs-md-viewer.
 *
 * Pipeline: stdin -> parse -> render -> viewport loop.
 * Reads markdown from stdin, parses it into an AST, renders to
 * styled display lines, and presents a scrollable pager in the terminal.
 */

#define _POSIX_C_SOURCE 200809L

#include "md_parse.h"
#include "md_render.h"
#include "md_viewport.h"
#include "md_terminal.h"
#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read all of stdin into a malloc'd NUL-terminated buffer.
 * Returns NULL on allocation failure. */
static char *read_stdin(void) {
    size_t cap = 65536;  /* 64 KB initial allocation */
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    for (;;) {
        size_t avail = cap - len;
        if (avail == 0) {
            size_t newcap = cap * 2;
            char *newbuf = realloc(buf, newcap);
            if (!newbuf) {
                free(buf);
                return NULL;
            }
            buf = newbuf;
            cap = newcap;
            avail = cap - len;
        }
        size_t n = fread(buf + len, 1, avail, stdin);
        len += n;
        if (n < avail) {
            /* EOF or error */
            if (ferror(stdin)) {
                free(buf);
                return NULL;
            }
            break;
        }
    }

    /* NUL-terminate, ensuring space */
    if (len == cap) {
        char *newbuf = realloc(buf, cap + 1);
        if (!newbuf) {
            free(buf);
            return NULL;
        }
        buf = newbuf;
    }
    buf[len] = '\0';
    return buf;
}

int main(int argc, char *argv[]) {
    int force_cols = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--width=", 8) == 0) {
            force_cols = atoi(argv[i] + 8);
        }
    }

    /* 1. Read all stdin */
    char *input = read_stdin();
    if (!input) {
        fprintf(stderr, "nbs-md-viewer: failed to read stdin\n");
        return 1;
    }
    if (input[0] == '\0') {
        free(input);
        return 0;
    }

    /* 2. Parse into AST */
    md_block_node_t *doc = md_parse(input);
    free(input);  /* parser made its own copies */

    /* 3. Enter raw mode first (opens /dev/tty for correct terminal queries) */
    if (md_terminal_enter_raw() != 0) {
        fprintf(stderr, "nbs-md-viewer: failed to enter raw mode\n");
        md_block_destroy(doc);
        return 1;
    }

    /* 4. Get terminal size (now uses /dev/tty via tty_fd) */
    int rows, cols;
    if (md_terminal_get_size(&rows, &cols) != 0) {
        rows = 24;
        cols = 80;
    }

    if (force_cols > 0) cols = force_cols;

    /* 5. Render AST to display lines at actual terminal width */
    md_layout_t *layout = md_render(doc, cols);

    /* 6. Initialize viewport and draw initial screen */
    md_view_state_t vs;
    memset(&vs, 0, sizeof(vs));
    md_viewport_init(&vs, rows, cols, layout->line_count);
    md_viewport_draw(&vs, layout);
    fflush(stdout);

    /* 7. Event loop */
    while (1) {
        /* Check for terminal resize */
        if (md_terminal_resize_pending()) {
            md_terminal_get_size(&rows, &cols);
            md_layout_destroy(layout);
            layout = md_render(doc, cols);
            int saved_offset = vs.scroll_offset;
            md_viewport_init(&vs, rows, cols, layout->line_count);
            vs.scroll_offset = saved_offset;
            /* Clamp scroll offset to new bounds */
            int max_off = vs.total_lines - vs.visible_rows;
            if (max_off < 0) max_off = 0;
            if (vs.scroll_offset > max_off) vs.scroll_offset = max_off;
            /* Clear entire screen and reset cursor for clean redraw */
            fputs("\033[2J\033[H", stdout);
            fflush(stdout);
            md_viewport_draw(&vs, layout);
            fflush(stdout);
        }

        md_key_t key = md_terminal_read_key();
        int need_draw = 1;

        switch (key) {
        case MD_KEY_QUIT:
            goto done;
        case MD_KEY_UP:
            md_viewport_scroll_up(&vs, 1);
            break;
        case MD_KEY_DOWN:
            md_viewport_scroll_down(&vs, 1);
            break;
        case MD_KEY_PAGE_UP:
            md_viewport_page_up(&vs);
            break;
        case MD_KEY_PAGE_DOWN:
            md_viewport_page_down(&vs);
            break;
        case MD_KEY_HOME:
            md_viewport_home(&vs);
            break;
        case MD_KEY_END:
            md_viewport_end(&vs);
            break;
        case MD_KEY_RIGHT:
            md_viewport_pan_right(&vs);
            break;
        case MD_KEY_LEFT:
            md_viewport_pan_left(&vs);
            break;
        default:
            need_draw = 0;
            break;
        }

        if (need_draw) {
            md_viewport_draw(&vs, layout);
            fflush(stdout);
        }
    }

done:
    md_terminal_leave_raw();
    md_layout_destroy(layout);
    md_block_destroy(doc);
    return 0;
}
