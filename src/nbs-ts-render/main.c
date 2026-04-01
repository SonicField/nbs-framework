/*
 * main.c — nbs-ts-render CLI entry point.
 *
 * Reads raw PTY output from stdin, processes it through the terminal
 * emulator, and outputs the final screen state as plain text on stdout.
 *
 * Usage: nbs-ts-render [--width=N] [--height=N] [--help]
 *        cat output.log | nbs-ts-render
 */

#include "nbs_ts_render.h"
#include "../nbs-common/nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Exit code for invalid arguments (matches NBS convention) */
#define NBS_TS_EXIT_BAD_ARGS 4

/* Read buffer size: 64KB */
#define READ_BUF_SIZE (64 * 1024)

static void print_help(void) {
    printf(
        "nbs-ts-render — Virtual terminal renderer\n"
        "\n"
        "Reads raw PTY output from stdin, processes it through a full\n"
        "terminal emulator (cursor movement, scrolling, erase), strips\n"
        "all decoration (color, bold, italic, underline), and outputs\n"
        "the final screen state as plain UTF-8 text.\n"
        "\n"
        "USAGE:\n"
        "    nbs-ts-render [OPTIONS]\n"
        "    cat output.log | nbs-ts-render\n"
        "    nbs-ts-render < output.log\n"
        "\n"
        "OPTIONS:\n"
        "    --width=N, --width N\n"
        "                  Set screen width in columns (default: %d)\n"
        "    --height=N, --height N\n"
        "                  Set screen height in rows (default: %d)\n"
        "    --no-strip    Preserve SGR colour/style escape sequences in output\n"
        "    --help        Show this help message and exit\n"
        "\n"
        "DESCRIPTION:\n"
        "    nbs-ts-render acts as a headless terminal emulator. It maintains\n"
        "    an internal screen buffer and processes escape sequences exactly\n"
        "    as a real terminal would. The output is what a human would see\n"
        "    on screen after all input has been processed.\n"
        "\n"
        "    The default dimensions (%dx%d) match the PTY size used by\n"
        "    nbs-ts-helper. Use --width and --height to match a different\n"
        "    terminal size.\n"
        "\n"
        "SUPPORTED ESCAPE SEQUENCES:\n"
        "    Cursor movement:  CUP, CUU, CUD, CUF, CUB, CNL, CPL, CHA,\n"
        "                      VPA, HVP\n"
        "    Erase:            ED (erase display), EL (erase line),\n"
        "                      ECH (erase characters)\n"
        "    Scroll:           SU (scroll up), SD (scroll down),\n"
        "                      DECSTBM (scroll region)\n"
        "    Insert/Delete:    IL, DL, ICH, DCH\n"
        "    Tabs:             HT, HTS, TBC\n"
        "    Cursor save:      DECSC (ESC 7), DECRC (ESC 8)\n"
        "    Line control:     LF, CR, BS, IND, NEL, RI\n"
        "    Reset:            RIS (ESC c)\n"
        "    UTF-8:            Full multi-byte character support\n"
        "\n"
        "    All SGR (color/style) sequences are silently stripped.\n"
        "    OSC and DCS sequences are silently consumed.\n"
        "\n"
        "EXAMPLES:\n"
        "    # Render an nbs-ts session log:\n"
        "    cat /tmp/nbs-ts-*/output.log | nbs-ts-render\n"
        "\n"
        "    # Render with custom terminal size:\n"
        "    cat output.log | nbs-ts-render --width=120 --height=40\n"
        "\n"
        "EXIT CODES:\n"
        "    0    Success\n"
        "    1    Runtime error (allocation failure, I/O error)\n"
        "    4    Bad arguments (invalid or missing option values)\n",
        NBS_TS_RENDER_DEFAULT_COLS, NBS_TS_RENDER_DEFAULT_ROWS,
        NBS_TS_RENDER_DEFAULT_COLS, NBS_TS_RENDER_DEFAULT_ROWS
    );
}

static int parse_int_arg(const char *arg, const char *prefix, int *out) {
    size_t plen = strlen(prefix);
    if (strncmp(arg, prefix, plen) != 0) return 0;
    const char *val = arg + plen;
    if (*val == '\0') {
        fprintf(stderr, "nbs-ts-render: missing value for %s\n", prefix);
        return -1;
    }
    char *end;
    errno = 0;
    long v = strtol(val, &end, 10);
    if (*end != '\0' || errno != 0 || v <= 0 || v > 10000) {
        fprintf(stderr, "nbs-ts-render: invalid value '%s' for %s (must be 1-10000)\n",
                val, prefix);
        return -1;
    }
    *out = (int)v;
    return 1;
}

int main(int argc, char *argv[]) {
    int width = NBS_TS_RENDER_DEFAULT_COLS;
    int height = NBS_TS_RENDER_DEFAULT_ROWS;
    int no_strip = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }

        int result;
        result = parse_int_arg(argv[i], "--width=", &width);
        if (result == -1) return NBS_TS_EXIT_BAD_ARGS;
        if (result == 1) continue;

        result = parse_int_arg(argv[i], "--height=", &height);
        if (result == -1) return NBS_TS_EXIT_BAD_ARGS;
        if (result == 1) continue;

        if (strcmp(argv[i], "--no-strip") == 0) {
            no_strip = 1;
            continue;
        }

        /* Support space-separated form: --width N / --height N */
        if (strcmp(argv[i], "--width") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "nbs-ts-render: --width requires a value\n");
                return NBS_TS_EXIT_BAD_ARGS;
            }
            char prefixed[64];
            snprintf(prefixed, sizeof(prefixed), "--width=%s", argv[++i]);
            result = parse_int_arg(prefixed, "--width=", &width);
            if (result == -1) return NBS_TS_EXIT_BAD_ARGS;
            continue;
        }

        if (strcmp(argv[i], "--height") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "nbs-ts-render: --height requires a value\n");
                return NBS_TS_EXIT_BAD_ARGS;
            }
            char prefixed[64];
            snprintf(prefixed, sizeof(prefixed), "--height=%s", argv[++i]);
            result = parse_int_arg(prefixed, "--height=", &height);
            if (result == -1) return NBS_TS_EXIT_BAD_ARGS;
            continue;
        }

        fprintf(stderr, "nbs-ts-render: unknown option '%s'\n"
                        "Try 'nbs-ts-render --help' for usage.\n", argv[i]);
        return NBS_TS_EXIT_BAD_ARGS;
    }

    ts_render_t *t = ts_render_create(height, width);
    if (!t) {
        fprintf(stderr, "nbs-ts-render: failed to allocate terminal buffer (%dx%d)\n",
                width, height);
        return 1;
    }

    if (no_strip) {
        ts_render_set_preserve_sgr(t, 1);
    }

    /* Read stdin in 64KB chunks and feed to emulator */
    char *buf = malloc(READ_BUF_SIZE);
    if (!buf) {
        fprintf(stderr, "nbs-ts-render: failed to allocate read buffer\n");
        ts_render_destroy(t);
        return 1;
    }

    size_t n;
    while ((n = fread(buf, 1, READ_BUF_SIZE, stdin)) > 0) {
        ts_render_feed(t, buf, n);
    }

    free(buf);

    if (ferror(stdin)) {
        fprintf(stderr, "nbs-ts-render: read error on stdin: %s\n", strerror(errno));
        ts_render_destroy(t);
        return 1;
    }

    /* Output final screen state */
    char *output = ts_render_snapshot(t);
    if (!output) {
        fprintf(stderr, "nbs-ts-render: failed to allocate snapshot buffer\n");
        ts_render_destroy(t);
        return 1;
    }

    fputs(output, stdout);

    free(output);
    ts_render_destroy(t);
    return 0;
}
