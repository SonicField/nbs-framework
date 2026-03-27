/*
 * nbs_term_attr.h — Shared terminal attribute abstraction.
 *
 * Abstracts xterm 256-colour codes and text attributes (bold, dim,
 * italic, underline, blink, inverse, strikethrough) so consumers
 * never emit raw escape sequences directly.
 *
 * Used by: nbs-chat-terminal, nbs-chat-edit (and any future terminal UI).
 *
 * Colour model:
 *   - 0..7    standard colours
 *   - 8..15   high-intensity colours
 *   - 16..231 6x6x6 RGB cube
 *   - 232..255 greyscale ramp
 *   - NBS_COLOUR_NONE (-1) = no colour set
 *
 * Thread safety: NOT thread-safe. All calls must be from the main thread.
 */

#ifndef NBS_TERM_ATTR_H
#define NBS_TERM_ATTR_H

#include <stdio.h>

/* --- Colour values --- */

#define NBS_COLOUR_NONE (-1)

/* --- Attribute flags (bitmask) --- */

#define NBS_ATTR_BOLD      (1u << 0)  /* SGR 1 */
#define NBS_ATTR_DIM       (1u << 1)  /* SGR 2 */
#define NBS_ATTR_ITALIC    (1u << 2)  /* SGR 3 */
#define NBS_ATTR_UNDERLINE (1u << 3)  /* SGR 4 */
#define NBS_ATTR_BLINK     (1u << 4)  /* SGR 5 */
#define NBS_ATTR_INVERSE   (1u << 5)  /* SGR 7 */
#define NBS_ATTR_STRIKE    (1u << 6)  /* SGR 9 */

/* --- Style struct --- */

typedef struct {
    int fg;            /* Foreground: 0-255 or NBS_COLOUR_NONE */
    int bg;            /* Background: 0-255 or NBS_COLOUR_NONE */
    unsigned attrs;    /* Bitmask of NBS_ATTR_* */
} nbs_style_t;

/* Initialiser: no colour, no attributes */
#define NBS_STYLE_INIT { NBS_COLOUR_NONE, NBS_COLOUR_NONE, 0 }

/* Minimum buffer size for nbs_style_start output.
 * Worst case: \033[1;2;3;4;5;7;9;38;5;255;48;5;255m = ~45 bytes + NUL */
#define NBS_STYLE_BUFSIZE 64

/* --- Predefined style constants --- */

/* Attribute-only styles */
extern const nbs_style_t NBS_STYLE_BOLD;
extern const nbs_style_t NBS_STYLE_DIM;
extern const nbs_style_t NBS_STYLE_REVERSE;
extern const nbs_style_t NBS_STYLE_STRIKE;

/* Semantic UI styles (256-colour foreground, no background, no attrs).
 * These replace the C_RED/C_GREEN/C_YELLOW/C_CYAN macros in editor.c
 * with proper 256-colour equivalents. */
extern const nbs_style_t NBS_STYLE_ERROR;     /* red fg (196) */
extern const nbs_style_t NBS_STYLE_WARNING;   /* yellow fg (226) */
extern const nbs_style_t NBS_STYLE_INFO;      /* cyan fg (87) */
extern const nbs_style_t NBS_STYLE_SUCCESS;   /* green fg (41) */

/* --- Escape sequence generation --- */

/*
 * nbs_style_start — Generate the CSI escape sequence to activate a style.
 *
 * Preconditions:
 *   - style != NULL
 *   - buf != NULL
 *   - bufsize >= 5 (minimum for any useful output)
 *
 * Postconditions:
 *   - On success: buf contains a NUL-terminated escape sequence (\033[...m).
 *     Returns the number of bytes written (excluding NUL), always > 0.
 *   - If style has no attrs and no colours (fully default): buf is set to ""
 *     and returns 0.
 *   - If bufsize is too small: returns -1, buf contents are undefined.
 *
 * The generated sequence is a single CSI with semicolon-separated SGR params.
 * Attribute codes are emitted first, then foreground, then background.
 */
int nbs_style_start(const nbs_style_t *style, char *buf, size_t bufsize);

/*
 * nbs_style_reset — Generate the CSI reset sequence (\033[0m).
 *
 * Preconditions:
 *   - buf != NULL
 *   - bufsize >= 5
 *
 * Postconditions:
 *   - On success: buf contains "\033[0m", returns 4.
 *   - If bufsize < 5: returns -1.
 */
int nbs_style_reset(char *buf, size_t bufsize);

/* --- Convenience: write directly to FILE* --- */

/*
 * nbs_style_fstart — Write style escape sequence to a FILE*.
 *
 * Preconditions:
 *   - style != NULL
 *   - out != NULL
 */
void nbs_style_fstart(const nbs_style_t *style, FILE *out);

/*
 * nbs_style_freset — Write reset sequence to a FILE*.
 *
 * Preconditions:
 *   - out != NULL
 */
void nbs_style_freset(FILE *out);

/* --- Handle-to-colour mapping --- */

/* Maximum number of handles that can be tracked */
#define NBS_MAX_HANDLE_COLOURS 256

/*
 * nbs_handle_colours_init — Reset the handle-to-colour mapping.
 *
 * Call before rendering a new session so colours are assigned
 * consistently from the start.
 */
void nbs_handle_colours_init(void);

/*
 * nbs_handle_colour — Get the style for a handle.
 *
 * Assigns a colour from the palette on first use, returns the same
 * style for the same handle on subsequent calls. Wraps around the
 * palette when exhausted.
 *
 * Preconditions:
 *   - handle != NULL
 *
 * Postconditions:
 *   - Returns a pointer to a nbs_style_t with a valid fg colour.
 *     The pointer is stable until nbs_handle_colours_init() is called.
 *   - Never returns NULL.
 */
const nbs_style_t *nbs_handle_colour(const char *handle);

/*
 * nbs_handle_palette_size — Return the number of entries in the palette.
 */
int nbs_handle_palette_size(void);

/*
 * nbs_handle_palette_entry — Return a palette entry by index.
 *
 * Preconditions:
 *   - index >= 0 && index < nbs_handle_palette_size()
 *
 * Returns NULL if index is out of range.
 */
const nbs_style_t *nbs_handle_palette_entry(int index);

#endif /* NBS_TERM_ATTR_H */
