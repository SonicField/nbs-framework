/*
 * md_highlight.h — Syntax highlighting for code fences.
 *
 * Provides a per-line tokeniser that tags spans with token types
 * (keyword, string, comment, etc.). Each language is registered
 * as an md_lang_t with its own tokenise callback. A cross-line
 * context allows multi-line constructs (block comments, heredocs).
 *
 * Token types map to styles via md_highlight_token_style().
 */

#ifndef MD_HIGHLIGHT_H
#define MD_HIGHLIGHT_H

#include "../nbs-common/nbs_term_attr.h"

/* --- Token types --- */

typedef enum {
    MD_HL_NORMAL = 0,   /* Plain code text */
    MD_HL_KEYWORD,      /* Language keyword (if, for, def, ...) */
    MD_HL_TYPE,         /* Type name (int, float, str, ...) */
    MD_HL_STRING,       /* String literal */
    MD_HL_NUMBER,       /* Numeric literal */
    MD_HL_COMMENT,      /* Comment (line or block) */
    MD_HL_PREPROC,      /* Preprocessor directive */
    MD_HL_OPERATOR,     /* Operator / punctuation */
} md_hl_token_t;

/* --- Cross-line context --- */

typedef enum {
    MD_HL_CTX_GROUND = 0,   /* Normal state */
    MD_HL_CTX_BLOCK_COMMENT,/* Inside a block comment */
    MD_HL_CTX_STRING,       /* Inside a multi-line string */
} md_hl_context_t;

/* --- Token span --- */

typedef struct {
    int            start;   /* Byte offset into the line */
    int            len;     /* Length in bytes */
    md_hl_token_t  token;   /* Token type */
} md_hl_span_t;

/* --- Language descriptor --- */

typedef struct md_lang md_lang_t;

/*
 * Tokenise callback signature:
 *   line     - NUL-terminated line of source code
 *   ctx      - in/out cross-line context
 *   spans    - output array (caller-allocated)
 *   max_spans - capacity of spans array
 *
 * Returns the number of spans written.
 */
typedef int (*md_tokenise_fn)(const char *line, md_hl_context_t *ctx,
                               md_hl_span_t *spans, int max_spans);

struct md_lang {
    const char      *name;      /* Canonical name (lowercase) */
    const char     **aliases;   /* NULL-terminated alias list */
    md_tokenise_fn   tokenise;  /* Line tokeniser */
};

/* --- Public API --- */

/*
 * md_highlight_find_lang — Look up a language by name or alias.
 *
 * Case-insensitive comparison. Returns NULL if not found.
 */
const md_lang_t *md_highlight_find_lang(const char *name);

/*
 * md_highlight_token_style — Map a token type to its display style.
 *
 * Returns a pointer to a static nbs_style_t.
 */
const nbs_style_t *md_highlight_token_style(md_hl_token_t token);

#endif /* MD_HIGHLIGHT_H */
