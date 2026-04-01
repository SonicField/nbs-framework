#ifndef MD_VIEWPORT_H
#define MD_VIEWPORT_H

#include "md_render.h"

typedef struct {
    int scroll_offset;   /* topmost visible line, >= 0 */
    int h_offset;        /* horizontal pan for wide lines, >= 0 */
    int visible_rows;    /* terminal height minus status bar (1 row) */
    int terminal_cols;   /* terminal width */
    int total_lines;     /* total display lines in layout */
    int resize_pending;  /* set when terminal size changes */
} md_view_state_t;

/* Initialize viewport state from terminal dimensions and layout. */
void md_viewport_init(md_view_state_t *vs, int rows, int cols, int total_lines);

/* Scroll up by n lines (clamped). */
void md_viewport_scroll_up(md_view_state_t *vs, int n);

/* Scroll down by n lines (clamped). */
void md_viewport_scroll_down(md_view_state_t *vs, int n);

/* Page up (visible_rows - 1). */
void md_viewport_page_up(md_view_state_t *vs);

/* Page down (visible_rows - 1). */
void md_viewport_page_down(md_view_state_t *vs);

/* Go to top. */
void md_viewport_home(md_view_state_t *vs);

/* Go to end. */
void md_viewport_end(md_view_state_t *vs);

/* Pan right by 4 columns. */
void md_viewport_pan_right(md_view_state_t *vs);

/* Pan left by 4 columns. */
void md_viewport_pan_left(md_view_state_t *vs);

/* Draw the current viewport to stdout. */
void md_viewport_draw(md_view_state_t *vs, md_layout_t *layout);

/* Draw a help screen showing key bindings. */
void md_viewport_draw_help(md_view_state_t *vs);

#endif /* MD_VIEWPORT_H */
