/*
 * md_style.h — Style palette for markdown rendering.
 *
 * Defines nbs_style_t constants for all markdown element types.
 * Extends the nbs_term_attr colour model (256-colour, attributes).
 */

#ifndef MD_STYLE_H
#define MD_STYLE_H

#include "../nbs-common/nbs_term_attr.h"

extern const nbs_style_t MD_STYLE_H1;
extern const nbs_style_t MD_STYLE_H2;
extern const nbs_style_t MD_STYLE_H3;
extern const nbs_style_t MD_STYLE_H4;
extern const nbs_style_t MD_STYLE_BODY;
extern const nbs_style_t MD_STYLE_BOLD;
extern const nbs_style_t MD_STYLE_ITALIC;
extern const nbs_style_t MD_STYLE_BOLD_ITALIC;
extern const nbs_style_t MD_STYLE_INLINE_CODE;
extern const nbs_style_t MD_STYLE_CODE_FENCE;
extern const nbs_style_t MD_STYLE_CODE_BORDER;
extern const nbs_style_t MD_STYLE_LINK_TEXT;
extern const nbs_style_t MD_STYLE_LINK_URL;
extern const nbs_style_t MD_STYLE_HRULE;
extern const nbs_style_t MD_STYLE_TABLE_BORDER;
extern const nbs_style_t MD_STYLE_TABLE_HEADER;
extern const nbs_style_t MD_STYLE_TABLE_CELL;
extern const nbs_style_t MD_STYLE_LIST_MARKER;
extern const nbs_style_t MD_STYLE_BLOCKQUOTE_BAR;
extern const nbs_style_t MD_STYLE_BLOCKQUOTE_TEXT;
extern const nbs_style_t MD_STYLE_STATUS_BAR;

/* Syntax highlighting */
extern const nbs_style_t MD_STYLE_HL_KEYWORD;
extern const nbs_style_t MD_STYLE_HL_TYPE;
extern const nbs_style_t MD_STYLE_HL_STRING;
extern const nbs_style_t MD_STYLE_HL_NUMBER;
extern const nbs_style_t MD_STYLE_HL_COMMENT;
extern const nbs_style_t MD_STYLE_HL_PREPROC;
extern const nbs_style_t MD_STYLE_HL_OPERATOR;

#endif /* MD_STYLE_H */
