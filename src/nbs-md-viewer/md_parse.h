/*
 * md_parse.h — Markdown parser.
 *
 * Two-pass parser: block structure first, then inline parsing.
 * Returns an AST rooted at a DOCUMENT node.
 */

#ifndef MD_PARSE_H
#define MD_PARSE_H

#include "md_ast.h"

/*
 * md_parse — Parse a markdown string into an AST.
 *
 * Returns a DOCUMENT block node owning the entire tree.
 * The caller must free with md_ast_destroy().
 * Never returns NULL (empty input produces an empty DOCUMENT).
 * Never crashes on any input.
 */
md_block_node_t *md_parse(const char *input);

#endif /* MD_PARSE_H */
