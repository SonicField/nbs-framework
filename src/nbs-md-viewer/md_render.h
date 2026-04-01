/*
 * md_render.h — AST to styled display lines.
 *
 * Walks the AST and produces a flat array of display lines.
 * Each line contains styled spans. The renderer handles paragraph
 * reflow, heading rendering, horizontal rules, list rendering,
 * code fences, tables, blockquotes, and block spacing.
 */

#ifndef MD_RENDER_H
#define MD_RENDER_H

#include "md_ast.h"
#include "../nbs-common/nbs_term_attr.h"

typedef struct {
    char        *text;      /* UTF-8 text content (owned) */
    nbs_style_t  style;     /* Colour + attributes */
    int          width;     /* Display width in columns */
} md_span_t;

typedef struct {
    md_span_t *spans;       /* Array of styled spans */
    int        span_count;
    int        display_width; /* Total display width in columns */
    int        source_block;  /* Which AST block produced this line */
    int        is_wide_line;  /* Whether this line supports h-pan */
} md_display_line_t;

typedef struct {
    md_display_line_t *lines;
    int                line_count;
    int                max_width;   /* Widest line — sets h-pan upper bound */
} md_layout_t;

/*
 * md_render — Render an AST to display lines.
 *
 * Produces a layout suitable for viewport display.
 * terminal_width: width for reflow and truncation.
 * The caller must free with md_layout_destroy().
 */
md_layout_t *md_render(md_block_node_t *root, int terminal_width);

/*
 * md_layout_destroy — Free a layout and all its display lines.
 */
void md_layout_destroy(md_layout_t *layout);

/*
 * md_layout_add_line — Add a display line to a layout (grows array).
 * Used internally by the renderer and table module.
 */
void md_layout_add_line(md_layout_t *layout, md_display_line_t *line);

/*
 * md_layout_add_blank — Add a blank display line with the given block id.
 */
void md_layout_add_blank(md_layout_t *layout, int block_id);

#endif /* MD_RENDER_H */
