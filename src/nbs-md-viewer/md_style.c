/*
 * md_style.c — Style palette for markdown rendering.
 *
 * All colours use the 256-colour model (indices 0-255).
 * See plan section 5.2 for rationale on each colour choice.
 */

#include "md_style.h"

const nbs_style_t MD_STYLE_H1             = { 223, 236, NBS_ATTR_BOLD };
const nbs_style_t MD_STYLE_H2             = { 180, 235, NBS_ATTR_BOLD };
const nbs_style_t MD_STYLE_H3             = { 110, NBS_COLOUR_NONE, NBS_ATTR_BOLD };
const nbs_style_t MD_STYLE_H4             = { 108, NBS_COLOUR_NONE, NBS_ATTR_BOLD | NBS_ATTR_DIM };
const nbs_style_t MD_STYLE_BODY           = { 253, NBS_COLOUR_NONE, 0 };
const nbs_style_t MD_STYLE_BOLD           = { 253, NBS_COLOUR_NONE, NBS_ATTR_BOLD };
const nbs_style_t MD_STYLE_ITALIC         = { 253, 233, NBS_ATTR_ITALIC };
const nbs_style_t MD_STYLE_BOLD_ITALIC    = { 253, 233, NBS_ATTR_BOLD | NBS_ATTR_ITALIC };
const nbs_style_t MD_STYLE_INLINE_CODE    = { 114, 235, 0 };
const nbs_style_t MD_STYLE_CODE_FENCE     = { 114, 234, 0 };
const nbs_style_t MD_STYLE_CODE_BORDER    = { 240, NBS_COLOUR_NONE, 0 };
const nbs_style_t MD_STYLE_LINK_TEXT      = { 176, NBS_COLOUR_NONE, NBS_ATTR_UNDERLINE };
const nbs_style_t MD_STYLE_HRULE          = { 240, NBS_COLOUR_NONE, 0 };
const nbs_style_t MD_STYLE_TABLE_BORDER   = { 240, NBS_COLOUR_NONE, 0 };
const nbs_style_t MD_STYLE_TABLE_HEADER   = { 223, 236, NBS_ATTR_BOLD };
const nbs_style_t MD_STYLE_TABLE_CELL     = { 253, NBS_COLOUR_NONE, 0 };
const nbs_style_t MD_STYLE_LIST_MARKER    = { 215, NBS_COLOUR_NONE, 0 };
const nbs_style_t MD_STYLE_BLOCKQUOTE_BAR = { 240, NBS_COLOUR_NONE, 0 };
const nbs_style_t MD_STYLE_BLOCKQUOTE_TEXT= { 250, NBS_COLOUR_NONE, NBS_ATTR_DIM };
const nbs_style_t MD_STYLE_STATUS_BAR     = { NBS_COLOUR_NONE, NBS_COLOUR_NONE, NBS_ATTR_INVERSE };

/* Syntax highlighting */
const nbs_style_t MD_STYLE_HL_KEYWORD    = { 173, 234, NBS_ATTR_BOLD };
const nbs_style_t MD_STYLE_HL_TYPE       = { 110, 234, 0 };
const nbs_style_t MD_STYLE_HL_STRING     = { 108, 234, 0 };
const nbs_style_t MD_STYLE_HL_NUMBER     = { 180, 234, 0 };
const nbs_style_t MD_STYLE_HL_COMMENT    = { 245, 234, NBS_ATTR_DIM };
const nbs_style_t MD_STYLE_HL_PREPROC    = { 183, 234, 0 };
const nbs_style_t MD_STYLE_HL_OPERATOR   = { 253, 234, 0 };
