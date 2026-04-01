/*
 * md_table.h — Table layout and box drawing.
 *
 * Provides column width calculation, alignment padding, and
 * Unicode box drawing character generation for table rendering.
 */

#ifndef MD_TABLE_H
#define MD_TABLE_H

#include "md_ast.h"
#include "md_render.h"

/*
 * md_table_render — Render a table block node into display lines.
 *
 * Appends display lines to the layout. Uses Unicode box drawing
 * characters (single-line borders, double-horizontal header separator).
 *
 * terminal_width: maximum width for truncation.
 * block_id: source block identifier for display lines.
 */
void md_table_render(md_layout_t *layout, md_block_node_t *table,
                     int terminal_width, int block_id);

#endif /* MD_TABLE_H */
