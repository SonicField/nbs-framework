/*
 * nbs_term_attr.c — Shared terminal attribute abstraction.
 *
 * See nbs_term_attr.h for API documentation and colour model.
 */

#include "nbs_term_attr.h"
#include "nbs_assert.h"

#include <string.h>
#include <stdio.h>

/* --- Predefined style constants --- */

const nbs_style_t NBS_STYLE_BOLD    = { NBS_COLOUR_NONE, NBS_COLOUR_NONE, NBS_ATTR_BOLD };
const nbs_style_t NBS_STYLE_DIM     = { NBS_COLOUR_NONE, NBS_COLOUR_NONE, NBS_ATTR_DIM };
const nbs_style_t NBS_STYLE_REVERSE = { NBS_COLOUR_NONE, NBS_COLOUR_NONE, NBS_ATTR_INVERSE };
const nbs_style_t NBS_STYLE_STRIKE  = { NBS_COLOUR_NONE, NBS_COLOUR_NONE, NBS_ATTR_STRIKE };

/* Semantic UI styles */
const nbs_style_t NBS_STYLE_ERROR   = { 196, NBS_COLOUR_NONE, 0 };  /* red */
const nbs_style_t NBS_STYLE_WARNING = { 226, NBS_COLOUR_NONE, 0 };  /* yellow */
const nbs_style_t NBS_STYLE_INFO    = {  87, NBS_COLOUR_NONE, 0 };  /* cyan */
const nbs_style_t NBS_STYLE_SUCCESS = {  41, NBS_COLOUR_NONE, 0 };  /* green */

/* Human message styles — dark grey (236) background strip */
const nbs_style_t NBS_STYLE_HUMAN_HANDLE    = { 223, 236, NBS_ATTR_BOLD };
const nbs_style_t NBS_STYLE_HUMAN_CONTENT   = { 253, 236, 0 };
const nbs_style_t NBS_STYLE_HUMAN_TIMESTAMP = { 245, 236, NBS_ATTR_DIM };
const nbs_style_t NBS_STYLE_HUMAN_PROMPT    = { 223, 236, NBS_ATTR_BOLD };

/* Medic warning — terracotta bold */
const nbs_style_t NBS_STYLE_MEDIC_WARNING   = { 173, NBS_COLOUR_NONE, NBS_ATTR_BOLD };

/* --- Escape sequence generation --- */

int nbs_style_start(const nbs_style_t *style, char *buf, size_t bufsize) {
    ASSERT_MSG(style != NULL, "nbs_style_start: style is NULL");
    ASSERT_MSG(buf != NULL, "nbs_style_start: buf is NULL");
    if (bufsize < 5) return -1;

    /* Check if there's anything to emit */
    if (style->attrs == 0 && style->fg == NBS_COLOUR_NONE && style->bg == NBS_COLOUR_NONE) {
        buf[0] = '\0';
        return 0;
    }

    /* Build the parameter list into a temp buffer.
     * Worst case: "1;2;3;4;5;7;9;38;5;255;48;5;255" = 34 chars */
    char params[48];
    int poff = 0;

    /* Attributes in ascending SGR code order */
    static const struct { unsigned mask; int code; } attr_table[] = {
        { NBS_ATTR_BOLD,      1 },
        { NBS_ATTR_DIM,       2 },
        { NBS_ATTR_ITALIC,    3 },
        { NBS_ATTR_UNDERLINE, 4 },
        { NBS_ATTR_BLINK,     5 },
        { NBS_ATTR_INVERSE,   7 },
        { NBS_ATTR_STRIKE,    9 },
    };

    for (int i = 0; i < (int)(sizeof(attr_table) / sizeof(attr_table[0])); i++) {
        if (style->attrs & attr_table[i].mask) {
            if (poff > 0) params[poff++] = ';';
            poff += snprintf(params + poff, sizeof(params) - (size_t)poff,
                             "%d", attr_table[i].code);
        }
    }

    /* Foreground colour */
    if (style->fg >= 0 && style->fg <= 255) {
        if (poff > 0) params[poff++] = ';';
        poff += snprintf(params + poff, sizeof(params) - (size_t)poff,
                         "38;5;%d", style->fg);
    }

    /* Background colour */
    if (style->bg >= 0 && style->bg <= 255) {
        if (poff > 0) params[poff++] = ';';
        poff += snprintf(params + poff, sizeof(params) - (size_t)poff,
                         "48;5;%d", style->bg);
    }

    /* Assemble: \033[ + params + m + NUL */
    int needed = 2 + poff + 1 + 1; /* ESC[ + params + m + NUL */
    if ((size_t)needed > bufsize) {
        return -1;
    }

    int n = snprintf(buf, bufsize, "\033[%sm", params);
    if (n < 0 || (size_t)n >= bufsize) {
        return -1;
    }
    return n;
}

int nbs_style_reset(char *buf, size_t bufsize) {
    ASSERT_MSG(buf != NULL, "nbs_style_reset: buf is NULL");
    if (bufsize < 5) return -1;
    memcpy(buf, "\033[0m", 5); /* includes NUL */
    return 4;
}

/* --- Convenience: FILE* output --- */

void nbs_style_fstart(const nbs_style_t *style, FILE *out) {
    ASSERT_MSG(style != NULL, "nbs_style_fstart: style is NULL");
    ASSERT_MSG(out != NULL, "nbs_style_fstart: out is NULL");
    char buf[NBS_STYLE_BUFSIZE];
    int n = nbs_style_start(style, buf, sizeof(buf));
    if (n > 0) {
        fwrite(buf, 1, (size_t)n, out);
    }
}

void nbs_style_freset(FILE *out) {
    ASSERT_MSG(out != NULL, "nbs_style_freset: out is NULL");
    fputs("\033[0m", out);
}

/* --- Handle-to-colour palette --- */

static const nbs_style_t PALETTE[] = {
    {  73, NBS_COLOUR_NONE, 0 },  /*  0: Soft teal     #5fafaf */
    { 180, NBS_COLOUR_NONE, 0 },  /*  1: Warm sand     #d7af87 */
    { 174, NBS_COLOUR_NONE, 0 },  /*  2: Muted rose    #d78787 */
    { 108, NBS_COLOUR_NONE, 0 },  /*  3: Pale sage     #87af87 */
    { 183, NBS_COLOUR_NONE, 0 },  /*  4: Soft lavender #d7afff */
    { 215, NBS_COLOUR_NONE, 0 },  /*  5: Warm amber    #ffaf5f */
    { 110, NBS_COLOUR_NONE, 0 },  /*  6: Steel blue    #87afd7 */
    { 209, NBS_COLOUR_NONE, 0 },  /*  7: Dusty coral   #ff875f */
    { 115, NBS_COLOUR_NONE, 0 },  /*  8: Soft mint     #87d7af */
    { 186, NBS_COLOUR_NONE, 0 },  /*  9: Pale gold     #d7d787 */
    { 182, NBS_COLOUR_NONE, 0 },  /* 10: Mauve         #d7afd7 */
    { 152, NBS_COLOUR_NONE, 0 },  /* 11: Powder blue   #afd7d7 */
    { 216, NBS_COLOUR_NONE, 0 },  /* 12: Peach         #ffaf87 */
    { 114, NBS_COLOUR_NONE, 0 },  /* 13: Spring green  #87d787 */
    { 146, NBS_COLOUR_NONE, 0 },  /* 14: Wisteria      #afafd7 */
    { 223, NBS_COLOUR_NONE, 0 },  /* 15: Cream         #ffd7af */
};
#define PALETTE_SIZE ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

#define MAX_HANDLE_LEN 64

typedef struct {
    char handle[MAX_HANDLE_LEN];
    int colour_index;
} handle_colour_entry_t;

static handle_colour_entry_t handle_map[NBS_MAX_HANDLE_COLOURS];
static int handle_count = 0;
static int next_colour = 0;

void nbs_handle_colours_init(void) {
    handle_count = 0;
    next_colour = 0;
}

const nbs_style_t *nbs_handle_colour(const char *handle) {
    ASSERT_MSG(handle != NULL, "nbs_handle_colour: handle is NULL");

    for (int i = 0; i < handle_count; i++) {
        if (strcmp(handle_map[i].handle, handle) == 0) {
            return &PALETTE[handle_map[i].colour_index];
        }
    }

    if (handle_count < NBS_MAX_HANDLE_COLOURS) {
        int sn = snprintf(handle_map[handle_count].handle,
                          MAX_HANDLE_LEN, "%s", handle);
        if (sn < 0 || sn >= MAX_HANDLE_LEN) {
            fprintf(stderr, "warning: handle truncated in colour table: "
                    "length %d exceeds %d — using default colour\n",
                    sn, MAX_HANDLE_LEN - 1);
            return &PALETTE[0];
        }
        handle_map[handle_count].colour_index = next_colour;
        handle_count++;

        int idx = next_colour;
        next_colour = (next_colour + 1) % PALETTE_SIZE;
        return &PALETTE[idx];
    }

    /* Overflow: return default palette entry */
    return &PALETTE[0];
}

int nbs_handle_palette_size(void) {
    return PALETTE_SIZE;
}

const nbs_style_t *nbs_handle_palette_entry(int index) {
    if (index < 0 || index >= PALETTE_SIZE) return NULL;
    return &PALETTE[index];
}
